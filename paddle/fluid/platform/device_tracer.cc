/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/platform/device_tracer.h"
#include <map>
#include <mutex>
#include "glog/logging.h"
#include "paddle/fluid/framework/block_desc.h"
#include "paddle/fluid/string/printf.h"

namespace paddle {
namespace platform {
namespace {

thread_local const char *cur_annotation = nullptr;
std::once_flag tracer_once_flag;
DeviceTracer *tracer = nullptr;
}  // namespace
#ifdef PADDLE_WITH_CUPTI

namespace {
// TODO(panyx0718): Revisit the buffer size here.
uint64_t kBufSize = 32 * 1024;
uint64_t kAlignSize = 8;

#define ALIGN_BUFFER(buffer, align)                                 \
  (((uintptr_t)(buffer) & ((align)-1))                              \
       ? ((buffer) + (align) - ((uintptr_t)(buffer) & ((align)-1))) \
       : (buffer))

#define CUPTI_CALL(call)                                                   \
  do {                                                                     \
    CUptiResult _status = call;                                            \
    if (_status != CUPTI_SUCCESS) {                                        \
      const char *errstr;                                                  \
      dynload::cuptiGetResultString(_status, &errstr);                     \
      fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n", \
              __FILE__, __LINE__, #call, errstr);                          \
      exit(-1);                                                            \
    }                                                                      \
  } while (0)

void EnableActivity() {
  // Device activity record is created when CUDA initializes, so we
  // want to enable it before cuInit() or any CUDA runtime call.
  CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY));
  CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_KERNEL));
  CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DEVICE));
  CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMSET));
  CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_OVERHEAD));
  // We don't track these activities for now.
  // CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONTEXT));
  // CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER));
  // CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME));
  // CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_NAME));
  // CUPTI_CALL(dynload::cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MARKER));
}

void DisableActivity() {
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_MEMCPY));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_KERNEL));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_DEVICE));
  // Disable all other activity record kinds.
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_CONTEXT));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_DRIVER));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_RUNTIME));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_MEMSET));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_NAME));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_MARKER));
  CUPTI_CALL(dynload::cuptiActivityDisable(CUPTI_ACTIVITY_KIND_OVERHEAD));
}

void CUPTIAPI bufferRequested(uint8_t **buffer, size_t *size,
                              size_t *maxNumRecords) {
  uint8_t *buf = (uint8_t *)malloc(kBufSize + kAlignSize);
  *size = kBufSize;
  *buffer = ALIGN_BUFFER(buf, kAlignSize);
  *maxNumRecords = 0;
}

void CUPTIAPI bufferCompleted(CUcontext ctx, uint32_t streamId, uint8_t *buffer,
                              size_t size, size_t validSize) {
  CUptiResult status;
  CUpti_Activity *record = NULL;
  if (validSize > 0) {
    do {
      status = dynload::cuptiActivityGetNextRecord(buffer, validSize, &record);
      if (status == CUPTI_SUCCESS) {
        switch (record->kind) {
          case CUPTI_ACTIVITY_KIND_KERNEL:
          case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
            auto *kernel =
                reinterpret_cast<const CUpti_ActivityKernel3 *>(record);
            tracer->AddKernelRecords(kernel->start, kernel->end,
                                     kernel->deviceId, kernel->streamId,
                                     kernel->correlationId);
            break;
          }
          default: { break; }
        }
      } else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED) {
        // Seems not an error in this case.
        break;
      } else {
        CUPTI_CALL(status);
      }
    } while (1);

    size_t dropped;
    CUPTI_CALL(
        dynload::cuptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
    if (dropped != 0) {
      fprintf(stderr, "Dropped %u activity records\n", (unsigned int)dropped);
    }
  }
  free(buffer);
}
}  // namespace

class DeviceTracerImpl : public DeviceTracer {
 public:
  DeviceTracerImpl() : enabled_(false) {}

  void AddAnnotation(uint64_t id, const std::string &anno) {
    std::lock_guard<std::mutex> l(trace_mu_);
    correlations_[id] = anno;
  }

  void AddKernelRecords(uint64_t start, uint64_t end, uint32_t device_id,
                        uint32_t stream_id, uint32_t correlation_id) {
    std::lock_guard<std::mutex> l(trace_mu_);
    kernel_records_.push_back(
        KernelRecord{start, end, device_id, stream_id, correlation_id});
  }

  bool IsEnabled() {
    std::lock_guard<std::mutex> l(trace_mu_);
    return enabled_;
  }

  void Enable() {
    std::lock_guard<std::mutex> l(trace_mu_);
    if (enabled_) {
      fprintf(stderr, "DeviceTracer already enabled\n");
      return;
    }
    EnableActivity();

    // Register callbacks for buffer requests and completed by CUPTI.
    CUPTI_CALL(dynload::cuptiActivityRegisterCallbacks(bufferRequested,
                                                       bufferCompleted));

    CUptiResult ret;
    ret = dynload::cuptiSubscribe(
        &subscriber_, static_cast<CUpti_CallbackFunc>(ApiCallback), this);
    if (ret == CUPTI_ERROR_MAX_LIMIT_REACHED) {
      fprintf(stderr, "CUPTI subcriber limit reached.\n");
    } else if (ret != CUPTI_SUCCESS) {
      fprintf(stderr, "Failed to create CUPTI subscriber.\n");
    }
    CUPTI_CALL(
        dynload::cuptiEnableCallback(1, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API,
                                     CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel));

    CUPTI_CALL(dynload::cuptiGetTimestamp(&start_ns_));
    enabled_ = true;
  }

  proto::Profile GenProfile() {
    std::lock_guard<std::mutex> l(trace_mu_);
    proto::Profile profile_pb;
    profile_pb.set_start_ns(start_ns_);
    profile_pb.set_end_ns(end_ns_);
    std::map<std::string, std::vector<uint64_t>> event_times;
    for (const KernelRecord &r : kernel_records_) {
      if (correlations_.find(r.correlation_id) == correlations_.end()) {
        fprintf(stderr, "cannot relate a kernel activity\n");
        continue;
      }
      auto *event = profile_pb.add_events();
      event->set_name(correlations_.at(r.correlation_id));
      event->set_start_ns(r.start_ns);
      event->set_end_ns(r.end_ns);
      event->set_stream_id(r.stream_id);
      event->set_device_id(r.device_id);
      event_times[event->name()].push_back(r.end_ns - r.start_ns);
    }
    for (const auto &et : event_times) {
      fprintf(
          stderr, "%s: total: %fms invoked cuda kernels: %lu\n",
          et.first.c_str(),
          std::accumulate(et.second.begin(), et.second.end(), 0) / 1000000.0,
          et.second.size());
    }
    return profile_pb;
  }

  void Disable() {
    // flush might cause additional calls to DeviceTracker.
    dynload::cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);
    std::lock_guard<std::mutex> l(trace_mu_);
    DisableActivity();
    dynload::cuptiUnsubscribe(subscriber_);
    CUPTI_CALL(dynload::cuptiGetTimestamp(&end_ns_));
    PADDLE_ENFORCE(dynload::cuptiFinalize());
    enabled_ = false;
  }

 private:
  static void CUPTIAPI ApiCallback(void *userdata, CUpti_CallbackDomain domain,
                                   CUpti_CallbackId cbid, const void *cbdata) {
    auto *cbInfo = reinterpret_cast<const CUpti_CallbackData *>(cbdata);
    DeviceTracer *tracer = reinterpret_cast<DeviceTracer *>(userdata);

    if ((domain == CUPTI_CB_DOMAIN_DRIVER_API) &&
        (cbid == CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel)) {
      if (cbInfo->callbackSite == CUPTI_API_ENTER) {
        const std::string anno =
            cur_annotation ? cur_annotation : cbInfo->symbolName;
        tracer->AddAnnotation(cbInfo->correlationId, anno);
      }
    } else {
      VLOG(1) << "Unhandled API Callback for " << domain << " " << cbid;
    }
  }

  std::mutex trace_mu_;
  bool enabled_;
  uint64_t start_ns_;
  uint64_t end_ns_;
  std::vector<KernelRecord> kernel_records_;
  std::unordered_map<uint32_t, std::string> correlations_;
  CUpti_SubscriberHandle subscriber_;
};

#endif  // PADDLE_WITH_CUPTI

class DeviceTracerDummy : public DeviceTracer {
 public:
  DeviceTracerDummy() {}

  void AddAnnotation(uint64_t id, const std::string &anno) {}

  void AddKernelRecords(uint64_t start, uint64_t end, uint32_t device_id,
                        uint32_t stream_id, uint32_t correlation_id) {}

  bool IsEnabled() { return false; }

  void Enable() {}

  proto::Profile GenProfile() { return proto::Profile(); }

  void Disable() {}
};

void CreateTracer(DeviceTracer **t) {
#ifdef PADDLE_WITH_CUPTI
  *t = new DeviceTracerImpl();
#else
  *t = new DeviceTracerDummy();
#endif  // PADDLE_WITH_CUPTI
}

DeviceTracer *GetDeviceTracer() {
  std::call_once(tracer_once_flag, CreateTracer, &tracer);
  return tracer;
}

void SetCurAnnotation(const char *anno) { cur_annotation = anno; }

void ClearCurAnnotation() { cur_annotation = nullptr; }

}  // namespace platform
}  // namespace paddle
