// Copyright 2020 Amazon Inc. or its affiliates. All Rights Reserved.
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
// =============================================================================

#include "vanilla_momentum.h"

#include "../../logging.h"

namespace byteps {
namespace common {
namespace compressor {
namespace {
CompressorRegistry::Register reg(
    "vanilla_momentum",
    [](const kwargs_t& kwargs) -> std::unique_ptr<BaseCompressor> {
      // register cpr
      auto ctor = CompressorRegistry::Find("error_feedback_type");
      BPS_CHECK_NE(ctor, nullptr);
      auto compressor_ptr = ctor(kwargs);
      // find \mu
      auto iter = kwargs.find("momentum_mu");
      BPS_CHECK_NE(iter, kwargs.end()) << "momentum \mu is not defined";
      float mu = std::stof(iter->second);
      BPS_LOG(DEBUG) << "with momentum";
      return std::unique_ptr<VanillaMomentumCompressor>(
          new VanillaMomentumCompressor(std::move(compressor_ptr), mu));
    });
}

VanillaMomentumCompressor::VanillaMomentumCompressor(
    std::unique_ptr<BaseCompressor> compressor_ptr, float mu)
    : Momentum(std::move(compressor_ptr), mu){};

VanillaMomentumCompressor::~VanillaMomentumCompressor() = default;

void VanillaMomentumCompressor::UpdateMom(ByteBuf grad, int dtype,
                                          ByteBuf& mom) {
  // m_{t} = \mu * m_{t-1} + g_t
  this->_cpu_reducer->sum(mom.data, grad.data, mom.data, grad.size,
                          static_cast<DataType>(dtype), _mu);
}
}  // namespace compressor
}  // namespace common
}  // namespace byteps