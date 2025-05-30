#include "taichi/runtime/gfx/runtime.h"
#include "taichi/program/program.h"
#include "taichi/common/filesystem.hpp"

// FIXME: (penguinliong) Special offer for `run_codegen`. Find a new home for it
// in the future.
#include "taichi/codegen/spirv/spirv_codegen.h"

#include <chrono>
#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fp16.h"

#define TI_RUNTIME_HOST
#include "taichi/program/context.h"
#undef TI_RUNTIME_HOST

namespace taichi::lang {
namespace gfx {

namespace {

class HostDeviceContextBlitter {
 public:
  HostDeviceContextBlitter(const KernelContextAttributes *ctx_attribs,
                           LaunchContextBuilder &host_ctx,
                           Device *device,
                           uint64_t *host_result_buffer,
                           DeviceAllocation *device_args_buffer,
                           DeviceAllocation *device_ret_buffer)
      : ctx_attribs_(ctx_attribs),
        host_ctx_(host_ctx),
        host_result_buffer_(host_result_buffer),
        device_args_buffer_(device_args_buffer),
        device_ret_buffer_(device_ret_buffer),
        device_(device) {
  }

  void host_to_device(
      const std::unordered_map<int, DeviceAllocation> &ext_arrays,
      const std::unordered_map<int, size_t> &ext_arr_size) {
    if (!ctx_attribs_->has_args()) {
      return;
    }

    void *device_base{nullptr};
    TI_ASSERT(device_->map(*device_args_buffer_, &device_base) ==
              RhiResult::success);

#define TO_DEVICE(short_type, type)               \
  if (arg.dtype == PrimitiveTypeID::short_type) { \
    auto d = host_ctx_.get_arg<type>(i);          \
    reinterpret_cast<type *>(device_ptr)[0] = d;  \
    break;                                        \
  }

    for (int i = 0; i < ctx_attribs_->args().size(); ++i) {
      const auto &arg = ctx_attribs_->args()[i];
      void *device_ptr = (uint8_t *)device_base + arg.offset_in_mem;
      do {
        if (arg.is_array) {
          if (host_ctx_.device_allocation_type[i] ==
                  LaunchContextBuilder::DevAllocType::kNone &&
              ext_arr_size.at(i)) {
            // Only need to blit ext arrs (host array)
            uint32_t access = uint32_t(ctx_attribs_->arr_access.at(i));
            if (access & uint32_t(irpass::ExternalPtrAccess::READ)) {
              DeviceAllocation buffer = ext_arrays.at(i);
              void *device_arr_ptr{nullptr};
              TI_ASSERT(device_->map(buffer, &device_arr_ptr) ==
                        RhiResult::success);
              const void *host_ptr = host_ctx_.array_ptrs[{i}];
              std::memcpy(device_arr_ptr, host_ptr, ext_arr_size.at(i));
              device_->unmap(buffer);
            }
          }
          // Substitute in the device address.

          // (penguinliong) We don't check the availability of physical pointer
          // here. It should be done before you need this class.
          if ((host_ctx_.device_allocation_type[i] ==
                   LaunchContextBuilder::DevAllocType::kNone ||
               host_ctx_.device_allocation_type[i] ==
                   LaunchContextBuilder::DevAllocType::kNdarray)) {
            uint64_t addr =
                device_->get_memory_physical_pointer(ext_arrays.at(i));
            reinterpret_cast<uint64 *>(device_ptr)[0] = addr;
          }
          // We should not process the rest
          break;
        }
        // (penguinliong) Same. The availability of short/long int types depends
        // on the kernels and compute graphs and the check should already be
        // done during module loads.
        TO_DEVICE(i8, int8)
        TO_DEVICE(u8, uint8)
        TO_DEVICE(i16, int16)
        TO_DEVICE(u16, uint16)
        TO_DEVICE(i32, int32)
        TO_DEVICE(u32, uint32)
        TO_DEVICE(f32, float32)
        TO_DEVICE(i64, int64)
        TO_DEVICE(u64, uint64)
        TO_DEVICE(f64, float64)
        if (arg.dtype == PrimitiveTypeID::f16) {
          auto d = fp16_ieee_from_fp32_value(host_ctx_.get_arg<float>(i));
          reinterpret_cast<uint16 *>(device_ptr)[0] = d;
          break;
        }
        TI_ERROR("Device does not support arg type={}",
                 PrimitiveType::get(arg.dtype).to_string());
      } while (false);
    }

    void *device_ptr =
        (uint8_t *)device_base + ctx_attribs_->extra_args_mem_offset();
    std::memcpy(device_ptr, host_ctx_.get_context().extra_args,
                ctx_attribs_->extra_args_bytes());

    device_->unmap(*device_args_buffer_);
#undef TO_DEVICE
  }

  bool device_to_host(
      CommandList *cmdlist,
      const std::unordered_map<int, DeviceAllocation> &ext_arrays,
      const std::unordered_map<int, size_t> &ext_arr_size) {
    if (ctx_attribs_->empty()) {
      return false;
    }

    bool require_sync = ctx_attribs_->rets().size() > 0;
    std::vector<DevicePtr> readback_dev_ptrs;
    std::vector<void *> readback_host_ptrs;
    std::vector<size_t> readback_sizes;

    for (int i = 0; i < ctx_attribs_->args().size(); ++i) {
      const auto &arg = ctx_attribs_->args()[i];
      if (arg.is_array &&
          host_ctx_.device_allocation_type[i] ==
              LaunchContextBuilder::DevAllocType::kNone &&
          ext_arr_size.at(i)) {
        uint32_t access = uint32_t(ctx_attribs_->arr_access.at(i));
        if (access & uint32_t(irpass::ExternalPtrAccess::WRITE)) {
          // Only need to blit ext arrs (host array)
          readback_dev_ptrs.push_back(ext_arrays.at(i).get_ptr(0));
          readback_host_ptrs.push_back(host_ctx_.array_ptrs[{i}]);
          readback_sizes.push_back(ext_arr_size.at(i));
          require_sync = true;
        }
      }
    }

    if (require_sync) {
      if (readback_sizes.size()) {
        StreamSemaphore command_complete_sema =
            device_->get_compute_stream()->submit(cmdlist);

        device_->wait_idle();

        // In this case `readback_data` syncs
        TI_ASSERT(device_->readback_data(
                      readback_dev_ptrs.data(), readback_host_ptrs.data(),
                      readback_sizes.data(), int(readback_sizes.size()),
                      {command_complete_sema}) == RhiResult::success);
      } else {
        device_->get_compute_stream()->submit_synced(cmdlist);
      }

      if (!ctx_attribs_->has_rets()) {
        return true;
      }
    } else {
      return false;
    }

    void *device_base{nullptr};
    TI_ASSERT(device_->map(*device_ret_buffer_, &device_base) ==
              RhiResult::success);

#define TO_HOST(short_type, type, offset)                            \
  if (dt->is_primitive(PrimitiveTypeID::short_type)) {               \
    const type d = *(reinterpret_cast<type *>(device_ptr) + offset); \
    host_result_buffer_[offset] =                                    \
        taichi_union_cast_with_different_sizes<uint64>(d);           \
    continue;                                                        \
  }

    for (int i = 0; i < ctx_attribs_->rets().size(); ++i) {
      // Note that we are copying the i-th return value on Metal to the i-th
      // *arg* on the host context.
      const auto &ret = ctx_attribs_->rets()[i];
      void *device_ptr = (uint8_t *)device_base + ret.offset_in_mem;
      const auto dt = PrimitiveType::get(ret.dtype);
      const auto num = ret.stride / data_type_size(dt);
      for (int j = 0; j < num; ++j) {
        // (penguinliong) Again, it's the module loader's responsibility to
        // check the data type availability.
        TO_HOST(i8, int8, j)
        TO_HOST(u8, uint8, j)
        TO_HOST(i16, int16, j)
        TO_HOST(u16, uint16, j)
        TO_HOST(i32, int32, j)
        TO_HOST(u32, uint32, j)
        TO_HOST(f32, float32, j)
        TO_HOST(i64, int64, j)
        TO_HOST(u64, uint64, j)
        TO_HOST(f64, float64, j)
        if (dt->is_primitive(PrimitiveTypeID::f16)) {
          const float d = fp16_ieee_to_fp32_value(
              *reinterpret_cast<uint16 *>(device_ptr) + j);
          host_result_buffer_[j] =
              taichi_union_cast_with_different_sizes<uint64>(d);
          continue;
        }
        TI_ERROR("Device does not support return value type={}",
                 data_type_name(PrimitiveType::get(ret.dtype)));
      }
    }
#undef TO_HOST

    device_->unmap(*device_ret_buffer_);

    return true;
  }

  static std::unique_ptr<HostDeviceContextBlitter> maybe_make(
      const KernelContextAttributes *ctx_attribs,
      LaunchContextBuilder &host_ctx,
      Device *device,
      uint64_t *host_result_buffer,
      DeviceAllocation *device_args_buffer,
      DeviceAllocation *device_ret_buffer) {
    if (ctx_attribs->empty()) {
      return nullptr;
    }
    return std::make_unique<HostDeviceContextBlitter>(
        ctx_attribs, host_ctx, device, host_result_buffer, device_args_buffer,
        device_ret_buffer);
  }

 private:
  const KernelContextAttributes *const ctx_attribs_;
  LaunchContextBuilder &host_ctx_;
  uint64_t *const host_result_buffer_;
  DeviceAllocation *const device_args_buffer_;
  DeviceAllocation *const device_ret_buffer_;
  Device *const device_;
};

}  // namespace

constexpr size_t kGtmpBufferSize = 1024 * 1024;
constexpr size_t kListGenBufferSize = 32 << 20;

// Info for launching a compiled Taichi kernel, which consists of a series of
// Unified Device API pipelines.

CompiledTaichiKernel::CompiledTaichiKernel(const Params &ti_params)
    : ti_kernel_attribs_(*ti_params.ti_kernel_attribs),
      device_(ti_params.device) {
  input_buffers_[BufferType::GlobalTmps] = ti_params.global_tmps_buffer;
  input_buffers_[BufferType::ListGen] = ti_params.listgen_buffer;

  // Compiled_structs can be empty if loading a kernel from an AOT module as
  // the SNode are not re-compiled/structured. In this case, we assume a
  // single root buffer size configured from the AOT module.
  for (int root = 0; root < ti_params.num_snode_trees; ++root) {
    BufferInfo buffer = {BufferType::Root, root};
    input_buffers_[buffer] = ti_params.root_buffers[root];
  }

  const auto arg_sz = ti_kernel_attribs_.ctx_attribs.args_bytes();
  const auto ret_sz = ti_kernel_attribs_.ctx_attribs.rets_bytes();

  args_buffer_size_ = arg_sz;
  ret_buffer_size_ = ret_sz;

  if (arg_sz) {
    args_buffer_size_ += ti_kernel_attribs_.ctx_attribs.extra_args_bytes();
  }

  const auto &task_attribs = ti_kernel_attribs_.tasks_attribs;
  const auto &spirv_bins = ti_params.spirv_bins;
  TI_ASSERT(task_attribs.size() == spirv_bins.size());

  for (int i = 0; i < task_attribs.size(); ++i) {
    PipelineSourceDesc source_desc{PipelineSourceType::spirv_binary,
                                   (void *)spirv_bins[i].data(),
                                   spirv_bins[i].size() * sizeof(uint32_t)};
    auto [vp, res] = ti_params.device->create_pipeline_unique(
        source_desc, task_attribs[i].name, ti_params.backend_cache);
    pipelines_.push_back(std::move(vp));
  }
}

const TaichiKernelAttributes &CompiledTaichiKernel::ti_kernel_attribs() const {
  return ti_kernel_attribs_;
}

size_t CompiledTaichiKernel::num_pipelines() const {
  return pipelines_.size();
}

size_t CompiledTaichiKernel::get_args_buffer_size() const {
  return args_buffer_size_;
}

size_t CompiledTaichiKernel::get_ret_buffer_size() const {
  return ret_buffer_size_;
}

Pipeline *CompiledTaichiKernel::get_pipeline(int i) {
  return pipelines_[i].get();
}

GfxRuntime::GfxRuntime(const Params &params)
    : device_(params.device),
      host_result_buffer_(params.host_result_buffer),
      profiler_(params.profiler) {
  TI_ASSERT(host_result_buffer_ != nullptr);
  current_cmdlist_pending_since_ = high_res_clock::now();
  init_nonroot_buffers();

  // Read pipeline cache from disk if available.
  std::filesystem::path cache_path(get_repo_dir());
  cache_path /= "rhi_cache.bin";
  std::vector<char> cache_data;
  if (std::filesystem::exists(cache_path)) {
    TI_TRACE("Loading pipeline cache from {}", cache_path.generic_string());
    std::ifstream cache_file(cache_path, std::ios::binary);
    cache_data.assign(std::istreambuf_iterator<char>(cache_file),
                      std::istreambuf_iterator<char>());
  } else {
    TI_TRACE("Pipeline cache not found at {}", cache_path.generic_string());
  }
  auto [cache, res] = device_->create_pipeline_cache_unique(cache_data.size(),
                                                            cache_data.data());
  if (res == RhiResult::success) {
    backend_cache_ = std::move(cache);
  }
}

GfxRuntime::~GfxRuntime() {
  synchronize();

  // Write pipeline cache back to disk.
  if (backend_cache_) {
    uint8_t *cache_data = (uint8_t *)backend_cache_->data();
    size_t cache_size = backend_cache_->size();
    if (cache_data) {
      std::filesystem::path cache_path =
          std::filesystem::path(get_repo_dir()) / "rhi_cache.bin";
      std::ofstream cache_file(cache_path, std::ios::binary | std::ios::trunc);
      std::ostreambuf_iterator<char> output_iterator(cache_file);
      std::copy(cache_data, cache_data + cache_size, output_iterator);
    }
    backend_cache_.reset();
  }

  {
    decltype(ti_kernels_) tmp;
    tmp.swap(ti_kernels_);
  }
  global_tmps_buffer_.reset();
  listgen_buffer_.reset();
}

GfxRuntime::KernelHandle GfxRuntime::register_taichi_kernel(
    GfxRuntime::RegisterParams reg_params) {
  CompiledTaichiKernel::Params params;
  params.ti_kernel_attribs = &(reg_params.kernel_attribs);
  params.num_snode_trees = reg_params.num_snode_trees;
  params.device = device_;
  params.root_buffers = {};
  for (int root = 0; root < root_buffers_.size(); ++root) {
    params.root_buffers.push_back(root_buffers_[root].get());
  }
  params.global_tmps_buffer = global_tmps_buffer_.get();
  params.listgen_buffer = listgen_buffer_.get();
  params.backend_cache = backend_cache_.get();

  for (int i = 0; i < reg_params.task_spirv_source_codes.size(); ++i) {
    const auto &spirv_src = reg_params.task_spirv_source_codes[i];

    // If we can reach here, we have succeeded. Otherwise
    // std::optional::value() would have killed us.
    params.spirv_bins.push_back(std::move(spirv_src));
  }
  KernelHandle res;
  res.id_ = ti_kernels_.size();
  ti_kernels_.push_back(std::make_unique<CompiledTaichiKernel>(params));
  return res;
}

void GfxRuntime::launch_kernel(KernelHandle handle,
                               LaunchContextBuilder &host_ctx) {
  auto *ti_kernel = ti_kernels_[handle.id_].get();

#if defined(__APPLE__)
  if (profiler_) {
    const int apple_max_query_pool_count = 32;
    int task_count = ti_kernel->ti_kernel_attribs().tasks_attribs.size();
    if (task_count > apple_max_query_pool_count) {
      TI_WARN(
          "Cannot concurrently profile more than 32 tasks in a single Taichi "
          "kernel. Profiling aborted.");
      profiler_ = nullptr;
    } else if (device_->profiler_get_sampler_count() + task_count >
               apple_max_query_pool_count) {
      flush();
      device_->profiler_sync();
    }
  }
#endif

  std::unique_ptr<DeviceAllocationGuard> args_buffer{nullptr},
      ret_buffer{nullptr};

  if (ti_kernel->get_args_buffer_size()) {
    auto [buf, res] = device_->allocate_memory_unique(
        {ti_kernel->get_args_buffer_size(),
         /*host_write=*/true, /*host_read=*/false,
         /*export_sharing=*/false, AllocUsage::Uniform});
    TI_ASSERT_INFO(res == RhiResult::success, "Failed to allocate args buffer");
    args_buffer = std::move(buf);
  }

  if (ti_kernel->get_ret_buffer_size()) {
    auto [buf, res] = device_->allocate_memory_unique(
        {ti_kernel->get_ret_buffer_size(),
         /*host_write=*/false, /*host_read=*/true,
         /*export_sharing=*/false, AllocUsage::Storage});
    TI_ASSERT_INFO(res == RhiResult::success, "Failed to allocate ret buffer");
    ret_buffer = std::move(buf);
  }

  // Create context blitter
  auto ctx_blitter = HostDeviceContextBlitter::maybe_make(
      &ti_kernel->ti_kernel_attribs().ctx_attribs, host_ctx, device_,
      host_result_buffer_, args_buffer.get(), ret_buffer.get());

  // `any_arrays` contain both external arrays and NDArrays
  std::unordered_map<int, DeviceAllocation> any_arrays;
  // `ext_array_size` only holds the size of external arrays (host arrays)
  // As buffer size information is only needed when it needs to be allocated
  // and transferred by the host
  std::unordered_map<int, size_t> ext_array_size;
  std::unordered_map<int, DeviceAllocation> textures;

  // Prepare context buffers & arrays
  if (ctx_blitter) {
    TI_ASSERT(ti_kernel->get_args_buffer_size() ||
              ti_kernel->get_ret_buffer_size());

    int i = 0;
    const auto &args = ti_kernel->ti_kernel_attribs().ctx_attribs.args();
    for (auto &arg : args) {
      if (arg.is_array) {
        if (host_ctx.device_allocation_type[i] !=
            LaunchContextBuilder::DevAllocType::kNone) {
          DeviceAllocation devalloc = kDeviceNullAllocation;

          // NDArray / Texture
          if (host_ctx.array_ptrs.count({i})) {
            devalloc = *(DeviceAllocation *)(host_ctx.array_ptrs[{i}]);
          }

          if (host_ctx.device_allocation_type[i] ==
              LaunchContextBuilder::DevAllocType::kNdarray) {
            any_arrays[i] = devalloc;
            ndarrays_in_use_.insert(devalloc.alloc_id);
          } else if (host_ctx.device_allocation_type[i] ==
                     LaunchContextBuilder::DevAllocType::kTexture) {
            textures[i] = devalloc;
          } else if (host_ctx.device_allocation_type[i] ==
                     LaunchContextBuilder::DevAllocType::kRWTexture) {
            textures[i] = devalloc;
          } else {
            TI_NOT_IMPLEMENTED;
          }
        } else {
          ext_array_size[i] = host_ctx.array_runtime_sizes[i];
          uint32_t access = uint32_t(
              ti_kernel->ti_kernel_attribs().ctx_attribs.arr_access.at(i));

          // Alloc ext arr
          size_t alloc_size = std::max(size_t(32), ext_array_size.at(i));
          bool host_write = access & uint32_t(irpass::ExternalPtrAccess::READ);
          auto [allocated, res] = device_->allocate_memory_unique(
              {alloc_size, host_write, false, /*export_sharing=*/false,
               AllocUsage::Storage});
          TI_ASSERT_INFO(res == RhiResult::success,
                         "Failed to allocate ext arr buffer");
          any_arrays[i] = *allocated.get();
          ctx_buffers_.push_back(std::move(allocated));
        }
      }
      i++;
    }

    ctx_blitter->host_to_device(any_arrays, ext_array_size);
  }

  ensure_current_cmdlist();

  // Record commands
  const auto &task_attribs = ti_kernel->ti_kernel_attribs().tasks_attribs;

  for (int i = 0; i < task_attribs.size(); ++i) {
    const auto &attribs = task_attribs[i];
    auto vp = ti_kernel->get_pipeline(i);
    const int group_x = (attribs.advisory_total_num_threads +
                         attribs.advisory_num_threads_per_group - 1) /
                        attribs.advisory_num_threads_per_group;
    std::unique_ptr<ShaderResourceSet> bindings =
        device_->create_resource_set_unique();
    for (auto &bind : attribs.buffer_binds) {
      // We might have to bind a invalid buffer (this is fine as long as
      // shader don't do anything with it)
      if (bind.buffer.type == BufferType::ExtArr) {
        bindings->rw_buffer(bind.binding, any_arrays.at(bind.buffer.root_id));
      } else if (bind.buffer.type == BufferType::Args) {
        bindings->buffer(bind.binding,
                         args_buffer ? *args_buffer : kDeviceNullAllocation);
      } else if (bind.buffer.type == BufferType::Rets) {
        bindings->rw_buffer(bind.binding,
                            ret_buffer ? *ret_buffer : kDeviceNullAllocation);
      } else {
        DeviceAllocation *alloc = ti_kernel->get_buffer_bind(bind.buffer);
        bindings->rw_buffer(bind.binding,
                            alloc ? *alloc : kDeviceNullAllocation);
      }
    }

    for (auto &bind : attribs.texture_binds) {
      DeviceAllocation texture = textures.at(bind.arg_id);
      if (bind.is_storage) {
        transition_image(texture, ImageLayout::shader_read_write);
        bindings->rw_image(bind.binding, texture, 0);
      } else {
        transition_image(texture, ImageLayout::shader_read);
        bindings->image(bind.binding, texture, {});
      }
    }

    if (attribs.task_type == OffloadedTaskType::listgen) {
      for (auto &bind : attribs.buffer_binds) {
        if (bind.buffer.type == BufferType::ListGen) {
          // FIXME: properlly support multiple list
          current_cmdlist_->buffer_fill(
              ti_kernel->get_buffer_bind(bind.buffer)->get_ptr(0),
              kBufferSizeEntireSize,
              /*data=*/0);
          current_cmdlist_->buffer_barrier(
              *ti_kernel->get_buffer_bind(bind.buffer));
        }
      }
    }

    current_cmdlist_->bind_pipeline(vp);
    RhiResult status = current_cmdlist_->bind_shader_resources(bindings.get());
    TI_ERROR_IF(status != RhiResult::success,
                "Resource binding error : RhiResult({})", status);

    if (profiler_) {
      current_cmdlist_->begin_profiler_scope(attribs.name);
    }

    status = current_cmdlist_->dispatch(group_x);

    if (profiler_) {
      current_cmdlist_->end_profiler_scope();
    }

    TI_ERROR_IF(status != RhiResult::success, "Dispatch error : RhiResult({})",
                status);
    current_cmdlist_->memory_barrier();
  }

  // Keep context buffers used in this dispatch
  if (ti_kernel->get_args_buffer_size()) {
    ctx_buffers_.push_back(std::move(args_buffer));
  }
  if (ti_kernel->get_ret_buffer_size()) {
    ctx_buffers_.push_back(std::move(ret_buffer));
  }

  // If we need to host sync, sync and remove in-flight references
  if (ctx_blitter) {
    if (ctx_blitter->device_to_host(current_cmdlist_.get(), any_arrays,
                                    ext_array_size)) {
      current_cmdlist_ = nullptr;
      ctx_buffers_.clear();
    }
  }

  submit_current_cmdlist_if_timeout();
}

void GfxRuntime::buffer_copy(DevicePtr dst, DevicePtr src, size_t size) {
  ensure_current_cmdlist();
  current_cmdlist_->buffer_copy(dst, src, size);
}

void GfxRuntime::copy_image(DeviceAllocation dst,
                            DeviceAllocation src,
                            const ImageCopyParams &params) {
  ensure_current_cmdlist();
  transition_image(dst, ImageLayout::transfer_dst);
  transition_image(src, ImageLayout::transfer_src);
  current_cmdlist_->copy_image(dst, src, ImageLayout::transfer_dst,
                               ImageLayout::transfer_src, params);
}

DeviceAllocation GfxRuntime::create_image(const ImageParams &params) {
  GraphicsDevice *gfx_device = dynamic_cast<GraphicsDevice *>(device_);
  TI_ERROR_IF(gfx_device == nullptr,
              "Image can only be created on a graphics device");
  DeviceAllocation image = gfx_device->create_image(params);
  track_image(image, ImageLayout::undefined);
  last_image_layouts_.at(image.alloc_id) = params.initial_layout;
  return image;
}

void GfxRuntime::track_image(DeviceAllocation image, ImageLayout layout) {
  last_image_layouts_[image.alloc_id] = layout;
}
void GfxRuntime::untrack_image(DeviceAllocation image) {
  last_image_layouts_.erase(image.alloc_id);
}
void GfxRuntime::transition_image(DeviceAllocation image, ImageLayout layout) {
  ImageLayout &last_layout = last_image_layouts_.at(image.alloc_id);
  ensure_current_cmdlist();
  current_cmdlist_->image_transition(image, last_layout, layout);
  last_layout = layout;
}

void GfxRuntime::synchronize() {
  flush();
  device_->wait_idle();
  // Profiler support
  if (profiler_) {
    device_->profiler_sync();
    auto sampled_records = device_->profiler_flush_sampled_time();
    for (auto &record : sampled_records) {
      profiler_->insert_record(record.first, record.second);
    }
  }
  ctx_buffers_.clear();
  ndarrays_in_use_.clear();
  fflush(stdout);
}

StreamSemaphore GfxRuntime::flush() {
  StreamSemaphore sema;
  if (current_cmdlist_) {
    sema = device_->get_compute_stream()->submit(current_cmdlist_.get());
    current_cmdlist_ = nullptr;
    ctx_buffers_.clear();
  } else {
    auto [cmdlist, res] =
        device_->get_compute_stream()->new_command_list_unique();
    TI_ASSERT(res == RhiResult::success);
    cmdlist->memory_barrier();
    sema = device_->get_compute_stream()->submit(cmdlist.get());
  }
  return sema;
}

Device *GfxRuntime::get_ti_device() const {
  return device_;
}

void GfxRuntime::ensure_current_cmdlist() {
  // Create new command list if current one is nullptr
  if (!current_cmdlist_) {
    current_cmdlist_pending_since_ = high_res_clock::now();
    auto [cmdlist, res] =
        device_->get_compute_stream()->new_command_list_unique();
    TI_ASSERT(res == RhiResult::success);
    current_cmdlist_ = std::move(cmdlist);
  }
}

void GfxRuntime::submit_current_cmdlist_if_timeout() {
  // If we have accumulated some work but does not require sync
  // and if the accumulated cmdlist has been pending for some time
  // launch the cmdlist to start processing.
  if (current_cmdlist_) {
    constexpr uint64_t max_pending_time = 2000;  // 2000us = 2ms
    auto duration = high_res_clock::now() - current_cmdlist_pending_since_;
    if (std::chrono::duration_cast<std::chrono::microseconds>(duration)
            .count() > max_pending_time) {
      flush();
    }
  }
}

void GfxRuntime::init_nonroot_buffers() {
  {
    auto [buf, res] = device_->allocate_memory_unique(
        {kGtmpBufferSize,
         /*host_write=*/false, /*host_read=*/false,
         /*export_sharing=*/false, AllocUsage::Storage});
    TI_ASSERT_INFO(res == RhiResult::success, "gtmp allocation failed");
    global_tmps_buffer_ = std::move(buf);
  }

  {
    auto [buf, res] = device_->allocate_memory_unique(
        {kListGenBufferSize,
         /*host_write=*/false, /*host_read=*/false,
         /*export_sharing=*/false, AllocUsage::Storage});
    TI_ASSERT_INFO(res == RhiResult::success, "listgen allocation failed");
    listgen_buffer_ = std::move(buf);
  }

  // Need to zero fill the buffers, otherwise there could be NaN.
  Stream *stream = device_->get_compute_stream();
  auto [cmdlist, res] =
      device_->get_compute_stream()->new_command_list_unique();
  TI_ASSERT(res == RhiResult::success);

  cmdlist->buffer_fill(global_tmps_buffer_->get_ptr(0), kBufferSizeEntireSize,
                       /*data=*/0);
  cmdlist->buffer_fill(listgen_buffer_->get_ptr(0), kBufferSizeEntireSize,
                       /*data=*/0);
  stream->submit_synced(cmdlist.get());
}

void GfxRuntime::add_root_buffer(size_t root_buffer_size) {
  if (root_buffer_size == 0) {
    root_buffer_size = 4;  // there might be empty roots
  }
  auto [new_buffer, res_buffer] = device_->allocate_memory_unique(
      {root_buffer_size,
       /*host_write=*/false, /*host_read=*/false,
       /*export_sharing=*/false, AllocUsage::Storage});
  TI_ASSERT_INFO(res_buffer == RhiResult::success,
                 "Failed to allocate root buffer");
  Stream *stream = device_->get_compute_stream();
  auto [cmdlist, res_cmdlist] =
      device_->get_compute_stream()->new_command_list_unique();
  TI_ASSERT(res_cmdlist == RhiResult::success);
  cmdlist->buffer_fill(new_buffer->get_ptr(0), kBufferSizeEntireSize,
                       /*data=*/0);
  stream->submit_synced(cmdlist.get());
  root_buffers_.push_back(std::move(new_buffer));
  // cache the root buffer size
  root_buffers_size_map_[root_buffers_.back().get()] = root_buffer_size;
}

DeviceAllocation *GfxRuntime::get_root_buffer(int id) const {
  if (id >= root_buffers_.size()) {
    TI_ERROR("root buffer id {} not found", id);
  }
  return root_buffers_[id].get();
}

size_t GfxRuntime::get_root_buffer_size(int id) const {
  auto it = root_buffers_size_map_.find(root_buffers_[id].get());
  if (id >= root_buffers_.size() || it == root_buffers_size_map_.end()) {
    TI_ERROR("root buffer id {} not found", id);
  }
  return it->second;
}

void GfxRuntime::enqueue_compute_op_lambda(
    std::function<void(Device *device, CommandList *cmdlist)> op,
    const std::vector<ComputeOpImageRef> &image_refs) {
  for (const auto &ref : image_refs) {
    TI_ASSERT(last_image_layouts_.find(ref.image.alloc_id) !=
              last_image_layouts_.end());
    transition_image(ref.image, ref.initial_layout);
  }

  ensure_current_cmdlist();
  op(device_, current_cmdlist_.get());

  for (const auto &ref : image_refs) {
    last_image_layouts_[ref.image.alloc_id] = ref.final_layout;
  }
}

GfxRuntime::RegisterParams run_codegen(
    Kernel *kernel,
    Arch arch,
    const DeviceCapabilityConfig &caps,
    const std::vector<CompiledSNodeStructs> &compiled_structs,
    const CompileConfig &compile_config) {
  const auto id = Program::get_kernel_id();
  const auto taichi_kernel_name(fmt::format("{}_k{:04d}_vk", kernel->name, id));
  TI_TRACE("VK codegen for Taichi kernel={}", taichi_kernel_name);
  spirv::KernelCodegen::Params params;
  params.ti_kernel_name = taichi_kernel_name;
  params.kernel = kernel;
  params.ir_root = kernel->ir.get();
  params.compiled_structs = compiled_structs;
  params.arch = arch;
  params.caps = caps;
  params.enable_spv_opt = compile_config.external_optimization_level > 0;
  spirv::KernelCodegen codegen(params);
  GfxRuntime::RegisterParams res;
  codegen.run(res.kernel_attribs, res.task_spirv_source_codes);
  res.num_snode_trees = compiled_structs.size();
  return res;
}

}  // namespace gfx
}  // namespace taichi::lang
