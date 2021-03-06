// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/backends/opencl/cl_include.h"
#include "lite/core/kernel.h"
#include "lite/core/op_registry.h"
#include "lite/kernels/opencl/image_helper.h"
#include "lite/operators/op_params.h"
#include "lite/utils/replace_stl/stream.h"

namespace paddle {
namespace lite {
namespace kernels {
namespace opencl {

class ConcatComputeImage : public KernelLite<TARGET(kOpenCL),
                                             PRECISION(kFP16),
                                             DATALAYOUT(kImageDefault)> {
 public:
  using param_t = operators::ConcatParam;

  void PrepareForRun() override {
    auto& context = ctx_->As<OpenCLContext>();
    concat_param_ = param_.get_mutable<param_t>();
    if (concat_param_->x.size() == 2) {
      kernel_func_name_ = "concat2";
    } else {
      kernel_func_name_ = "concat_mul";
    }
    context.cl_context()->AddKernel(
        kernel_func_name_, "image/concat_kernel.cl", build_options_);

    auto axis = concat_param_->axis;
    auto inputs = concat_param_->x;
    auto out_dims = concat_param_->output->dims();
    auto* axis_tensor = concat_param_->axis_tensor;
    if (axis_tensor != nullptr) {
      // auto* axis_tensor_data = axis_tensor->data<int>(TARGET(kARM));
      // axis = axis_tensor_data[0];
    }
    auto in_dims = inputs[0]->dims();
    axis_size_ = out_dims[axis];
    axis_ = axis;
    for (int i = 0; i < axis; i++) {
      pre_size_ *= in_dims[i];
    }
    for (int i = axis + 1; i < in_dims.size(); i++) {
      post_size_ *= in_dims[i];
    }
    for (int i = 1; i < inputs.size(); i++) {
      auto dims = inputs[i]->dims();
      // auto flag = CHECK_EQ_OR_FALSE(in_dims.size(), dims.size());
      if (in_dims.size() != dims.size()) {
        printf("input shape must be same \n");
        return;
      }
      for (int i = 0; i < dims.size(); i++) {
        if (i != axis) {
          if (in_dims[i] != dims[i]) {
            printf("input shape must be same \n");
            return;
          }
        }
      }
    }
  }

  void Run() override {
    auto& param = *param_.get_mutable<param_t>();
    const auto& x_dims = param.output->dims();
    auto image_shape = InitImageDimInfoWith(x_dims);
    auto* out_buf = param.output->mutable_data<uint16_t, cl::Image2D>(
        image_shape["width"], image_shape["height"]);
    const auto& y_dims = param.output->dims();  // useless: check dim only

    auto& context = ctx_->As<OpenCLContext>();
    CHECK(context.cl_context() != nullptr);
    STL::stringstream kernel_key;
    kernel_key << kernel_func_name_ << build_options_;

    auto inputs = param.x;
    int arg_idx = 0;
    int width = inputs[0]->dims()[-1];
    auto global_work_size =
        cl::NDRange{static_cast<cl::size_type>(image_shape["width"]),
                    static_cast<cl::size_type>(image_shape["height"])};
    VLOG(4) << TargetToStr(param.output->target());
    VLOG(4) << "image_shape(w,h):" << image_shape["width"] << " "
            << image_shape["height"];
    VLOG(4) << "x_dims[" << x_dims.size() << "D]:" << x_dims[0] << " "
            << x_dims[1] << " " << x_dims[2] << " " << x_dims[3];
    VLOG(4) << "y_dims[" << y_dims.size() << "D]:" << y_dims[0] << " "
            << y_dims[1] << " " << y_dims[2] << " " << y_dims[3];
    auto kernel = context.cl_context()->GetKernel(kernel_key.str());
    int flag = 1;  // cxw
    switch (axis_) {
      case 0:
        width = x_dims[2];  // n
        flag = 0;
        break;
      case 1:
        width = x_dims[3];  // c
        break;
      case 2:
        width = x_dims[0];  // h
        flag = 0;
        break;
      case 3:
      case -1:
        width = x_dims[1];  // w
        break;
      default:
        printf("this axis: %d does not support \n", axis_);
    }
    if (inputs.size() == 2) {
      auto* x_buf0 = inputs[0]->data<uint16_t, cl::Image2D>();
      auto* x_buf1 = inputs[1]->data<uint16_t, cl::Image2D>();
      cl_int status = kernel.setArg(arg_idx, *x_buf0);
      CL_CHECK_FATAL(status);
      status = kernel.setArg(++arg_idx, *x_buf1);
      CL_CHECK_FATAL(status);
      status = kernel.setArg(++arg_idx, *out_buf);
      CL_CHECK_FATAL(status);
      status =
          kernel.setArg(++arg_idx, static_cast<int>(inputs[0]->dims()[axis_]));
      CL_CHECK_FATAL(status);
      status = kernel.setArg(++arg_idx, flag);
      CL_CHECK_FATAL(status);
      status = kernel.setArg(++arg_idx, width);
      CL_CHECK_FATAL(status);
      status = context.cl_context()->GetCommandQueue().enqueueNDRangeKernel(
          kernel,
          cl::NullRange,
          global_work_size,
          cl::NullRange,
          nullptr,
          event_.get());
      CL_CHECK_FATAL(status);
      context.cl_context()->GetCommandQueue().finish();
    } else {
      auto start = 0;
      for (int i = 0; i < inputs.size(); i++) {
        arg_idx = 0;
        auto* x_buf = inputs[i]->data<uint16_t, cl::Image2D>();
        cl_int status = kernel.setArg(arg_idx, *x_buf);
        CL_CHECK_FATAL(status);
        status = kernel.setArg(++arg_idx, *out_buf);
        CL_CHECK_FATAL(status);
        status = kernel.setArg(++arg_idx, axis_size_);
        CL_CHECK_FATAL(status);
        status = kernel.setArg(++arg_idx, start);
        CL_CHECK_FATAL(status);
        status = kernel.setArg(++arg_idx, flag);
        CL_CHECK_FATAL(status);
        status = kernel.setArg(++arg_idx, width);
        CL_CHECK_FATAL(status);
        CL_CHECK_FATAL(status);
        status = context.cl_context()->GetCommandQueue().enqueueNDRangeKernel(
            kernel,
            cl::NullRange,
            global_work_size,
            cl::NullRange,
            nullptr,
            event_.get());
        CL_CHECK_FATAL(status);
        context.cl_context()->GetCommandQueue().finish();
        start += inputs[i]->dims()[axis_];
      }
    }
  }

  std::string doc() { return "Concat using cl::Image, kFP16"; }

  int axis_size_ = 1;
  int post_size_ = 1;
  int pre_size_ = 1;
  int axis_ = 1;
  param_t* concat_param_{nullptr};
  std::string kernel_func_name_{};
  std::string build_options_{"-DCL_DTYPE_half"};
  std::shared_ptr<cl::Event> event_{new cl::Event};
};

}  // namespace opencl
}  // namespace kernels
}  // namespace lite
}  // namespace paddle

typedef paddle::lite::kernels::opencl::ConcatComputeImage Concat_image;

REGISTER_LITE_KERNEL(
    concat, kOpenCL, kFP16, kImageDefault, Concat_image, ImageDefault)
    .BindInput("X",
               {LiteType::GetTensorTy(TARGET(kOpenCL),
                                      PRECISION(kFP16),
                                      DATALAYOUT(kImageDefault))})
    .BindInput("AxisTensor",
               {LiteType::GetTensorTy(TARGET(kOpenCL),
                                      PRECISION(kInt32),
                                      DATALAYOUT(kImageDefault))})
    .BindOutput("Out",
                {LiteType::GetTensorTy(TARGET(kOpenCL),
                                       PRECISION(kFP16),
                                       DATALAYOUT(kImageDefault))})
    .Finalize();
