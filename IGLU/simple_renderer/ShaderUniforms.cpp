/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ShaderUniforms.h"

#include <igl/Buffer.h>
#include <igl/Device.h>
#include <igl/Log.h>
#include <igl/Uniform.h>
#if IGL_BACKEND_OPENGL
#include <igl/opengl/RenderCommandEncoder.h>
#include <igl/opengl/RenderPipelineState.h>
#endif
#if defined(IGL_CMAKE_BUILD)
#include <igl/IGLSafeC.h>
#else
#include <secure_lib/secure_string.h>
#endif

#include <cstdlib>
#include <memory>
#include <string>

namespace {

// For Suballocated uniform buffers, try to allocate at most a buffer of size 64K.
// We will clamp the size to the limits of the device.
// For example, on the Quest 2 GPU, maxUniformBufferSize is 64k, so we are using it all.
constexpr size_t MAX_SUBALLOCATED_BUFFER_SIZE_BYTES = 65536;

uint8_t bindTargetForShaderStage(igl::ShaderStage stage) {
  switch (stage) {
  case igl::ShaderStage::Vertex:
    return igl::BindTarget::kVertex;
  case igl::ShaderStage::Fragment:
    return igl::BindTarget::kFragment;
  default:
    IGL_ASSERT_MSG(0, "invalid shader stage for rendering: %d", (int)stage);
    return 0;
  }
}

} // namespace

namespace iglu::material {

ShaderUniforms::ShaderUniforms(igl::IDevice& device,
                               const igl::IRenderPipelineReflection& reflection) :
  _backend(device.getBackendType()) {
  bool hasBindBytesFeature = device.hasFeature(igl::DeviceFeatures::BindBytes);
  size_t bindBytesLimit = 0;
  if (!device.getFeatureLimits(igl::DeviceFeatureLimits::MaxBindBytesBytes, bindBytesLimit)) {
    IGL_LOG_ERROR("[IGL][Warning] Failed to get MaxBindBytesBytes value. Turning off bind bytes");
    hasBindBytesFeature = false;
  }

  size_t uniformBufferLimit = 0;
  device.getFeatureLimits(igl::DeviceFeatureLimits::MaxUniformBufferBytes, uniformBufferLimit);

  for (const igl::BufferArgDesc& iglDesc : reflection.allUniformBuffers()) {
    bool isSuballocated = (_backend == igl::BackendType::Vulkan) ? true : false;

    size_t length = iglDesc.bufferDataSize;
    IGL_ASSERT_MSG(length > 0, "unexpected buffer with size 0");
    IGL_ASSERT_MSG(length <= MAX_SUBALLOCATED_BUFFER_SIZE_BYTES &&
                       (uniformBufferLimit == 0 || length <= uniformBufferLimit),
                   "buffer size exceeds limits");
    const size_t bufferAllocationLength =
        std::min(isSuballocated ? MAX_SUBALLOCATED_BUFFER_SIZE_BYTES : length,
                 uniformBufferLimit != 0 ? uniformBufferLimit : std::numeric_limits<size_t>::max());
    const std::string vertexBufferPrefix = "vertexBuffer.";
    if (device.getBackendType() == igl::BackendType::Metal &&
        iglDesc.name.toString().substr(0, vertexBufferPrefix.length()) == vertexBufferPrefix) {
      continue;
    }

    bool createBuffer = false;
    if (_backend == igl::BackendType::OpenGL) {
      // On OpenGL, create buffers only when dealing with uniform blocks (and not single uniforms)
      createBuffer = iglDesc.isUniformBlock;
    } else if (_backend == igl::BackendType::Vulkan) {
      createBuffer = true;
    } else {
      // On Metal, need to create buffers only when data > 4kb
      createBuffer = !hasBindBytesFeature || length > bindBytesLimit;
    }

    std::shared_ptr<igl::IBuffer> buffer = nullptr;
    if (createBuffer) {
      igl::BufferDesc desc;
      desc.length = bufferAllocationLength;
      desc.data = nullptr;
      desc.storage = igl::ResourceStorage::Shared;
      desc.type = igl::BufferDesc::BufferTypeBits::Uniform;
      desc.hint = igl::BufferDesc::BufferAPIHintBits::UniformBlock;
      if (_backend == igl::BackendType::Metal || _backend == igl::BackendType::Vulkan) {
        desc.hint |= igl::BufferDesc::BufferAPIHintBits::Ring;
      }
      buffer = device.createBuffer(desc, nullptr);
    } else {
      buffer = nullptr;
    }

    // All uniform updates will be made to this malloc'ed data block,
    // which will later be uploaded to the buffer (if using buffer)
    void* data = malloc(bufferAllocationLength);
    if (data == nullptr) {
      continue;
    }
    auto allocation = std::make_shared<BufferAllocation>(data, bufferAllocationLength, buffer);
    _allocations.push_back(allocation);

    std::shared_ptr<BufferDesc> bufferDesc = std::make_shared<BufferDesc>();
    bufferDesc->iglBufferDesc = iglDesc;
    bufferDesc->allocation = allocation;

    if (isSuballocated) {
      bufferDesc->isSuballocated = true;
      bufferDesc->suballocationsSize = length;
    }

    for (const igl::BufferArgDesc::BufferMemberDesc& uniformDesc : iglDesc.members) {
      UniformDesc uniform{uniformDesc, bufferDesc};
      _allUniformsByName.insert({uniformDesc.name, uniform});
      bufferDesc->uniforms.push_back(uniform);
    }

    _allBuffersByName[{iglDesc.name, iglDesc.shaderStage}] = bufferDesc;
    _bufferDescs.push_back(std::move(bufferDesc));
  }

  for (const igl::TextureArgDesc& iglDesc : reflection.allTextures()) {
    IGL_ASSERT_MSG(_allTexturesByName.find(iglDesc.name) == _allTexturesByName.end(),
                   "Texture names must be unique across all shader stages: %s",
                   iglDesc.name.c_str());
    _textureDescs.push_back(iglDesc);
    _allTexturesByName[iglDesc.name] = TextureSlot{nullptr, nullptr};
  }
}

ShaderUniforms::~ShaderUniforms() {
  for (auto& allocation : _allocations) {
    free(allocation->ptr);
  }
}

const igl::BufferArgDesc& ShaderUniforms::bufferDescriptor(const igl::NameHandle& bufferName,
                                                           igl::ShaderStage stage) const {
  auto it = _allBuffersByName.find({bufferName, stage});
  if (it != _allBuffersByName.end()) {
    return it->second->iglBufferDesc;
  }
  IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid buffer name for shader stage: %s",
                     bufferName.toConstChar());
  static igl::BufferArgDesc invalid;
  return invalid;
}

namespace {
size_t getUniformExpectedSize(igl::UniformType uniformType, igl::BackendType backend) {
  auto expectedSize = igl::sizeForUniformType(uniformType);
  if (backend == igl::BackendType::Metal || backend == igl::BackendType::Vulkan) {
    if (uniformType == igl::UniformType::Mat3x3) {
      expectedSize = 48;
    } else if (uniformType == igl::UniformType::Float3) {
      expectedSize = 16;
    }
  }

  return expectedSize;
}
} // namespace

void ShaderUniforms::setUniformBytes(const igl::NameHandle& name,
                                     const void* data,
                                     size_t elementSize,
                                     size_t count,
                                     size_t arrayIndex) {
  auto range = _allUniformsByName.equal_range(name);
  if (range.first == range.second) {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid uniform name: %s\n", name.toConstChar());
    return;
  }
  for (auto it = range.first; it != range.second; ++it) {
    auto& uniformDesc = it->second;
    if (_backend != igl::BackendType::Vulkan) {
      auto expectedSize = getUniformExpectedSize(uniformDesc.iglMemberDesc.type, _backend);
      if (elementSize != expectedSize) {
        IGL_LOG_ERROR_ONCE("[IGL][Error] Uniform size mismatch: %s : expected %d got %d\n",
                           name.toConstChar(),
                           expectedSize,
                           elementSize);
        continue;
      }
    }
    if (arrayIndex + count > uniformDesc.iglMemberDesc.arrayLength) {
      IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid range for uniform: %s - %zu,%zu,%zu\n",
                         name.toConstChar(),
                         arrayIndex,
                         count,
                         uniformDesc.iglMemberDesc.arrayLength);
      continue;
    }
    auto strongBuffer = uniformDesc.buffer.lock();
    if (!strongBuffer) {
      IGL_LOG_ERROR("[IGL][Error] null uniform buffer!");
      continue;
    }

    uintptr_t subAllocatedOffset = 0;
    if (strongBuffer->isSuballocated && strongBuffer->currentAllocation >= 0) {
      subAllocatedOffset = strongBuffer->currentAllocation * strongBuffer->suballocationsSize;
    }
    uintptr_t offset =
        uniformDesc.iglMemberDesc.offset + elementSize * arrayIndex + subAllocatedOffset;

    auto err = try_checked_memcpy((uint8_t*)strongBuffer->allocation->ptr + offset, // destination
                                  strongBuffer->allocation->size - offset, // max destination size
                                  data, // source
                                  elementSize * count // num bytes to copy
    );
    if (err != 0) {
      IGL_LOG_ERROR("[IGL][Error] Failed to update uniform buffer\n");
      continue;
    }
  }
}

void ShaderUniforms::setBool(const igl::NameHandle& uniformName,
                             const bool& value,
                             size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(bool), 1, arrayIndex);
}

void ShaderUniforms::setBoolArray(const igl::NameHandle& uniformName,
                                  bool* value,
                                  size_t count,
                                  size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(bool), count, arrayIndex);
}

void ShaderUniforms::setFloat(const igl::NameHandle& uniformName,
                              const iglu::simdtypes::float1& value,
                              size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float1), 1, arrayIndex);
}

void ShaderUniforms::setFloatArray(const igl::NameHandle& uniformName,
                                   iglu::simdtypes::float1* value,
                                   size_t count,
                                   size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float1), count, arrayIndex);
}

void ShaderUniforms::setFloat2(const igl::NameHandle& uniformName,
                               const iglu::simdtypes::float2& value,
                               size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float2), 1, arrayIndex);
}

void ShaderUniforms::setFloat2Array(const igl::NameHandle& uniformName,
                                    iglu::simdtypes::float2* value,
                                    size_t count,
                                    size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float2), count, arrayIndex);
}

void ShaderUniforms::setFloat3(const igl::NameHandle& uniformName,
                               const iglu::simdtypes::float3& value,
                               size_t arrayIndex) {
  size_t length = _backend == igl::BackendType::Metal ? sizeof(iglu::simdtypes::float3)
                                                      : sizeof(float[3]);
  setUniformBytes(uniformName, &value, length, 1, arrayIndex);
}

void ShaderUniforms::setFloat3Array(const igl::NameHandle& uniformName,
                                    iglu::simdtypes::float3* value,
                                    size_t count,
                                    size_t arrayIndex) {
  if (_backend == igl::BackendType::Metal) {
    setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float3), count, arrayIndex);
  } else {
    // simdtypes::float3 is padded to have an extra float.
    // Remove it so we can send the packed version to OpenGL/Vulkan
    auto* packedArray = new float[3 * count];
    float* packedArrayPtr = packedArray;
    auto paddedArrayPtr = reinterpret_cast<const float*>(value);
    for (size_t i = 0; i < count; i++) {
      for (int j = 0; j < 3; j++) {
        *packedArrayPtr++ = *paddedArrayPtr++;
      }
      paddedArrayPtr++; // padded float
    }
    setUniformBytes(uniformName, packedArray, sizeof(float) * 3 * count, 1, arrayIndex);
    delete[] packedArray;
  }
}

void ShaderUniforms::setFloat4(const igl::NameHandle& uniformName,
                               const iglu::simdtypes::float4& value,
                               size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float4), 1, arrayIndex);
}

void ShaderUniforms::setFloat4Array(const igl::NameHandle& uniformName,
                                    const iglu::simdtypes::float4* value,
                                    size_t count,
                                    size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float4), count, arrayIndex);
}

void ShaderUniforms::setFloat2x2(const igl::NameHandle& uniformName,
                                 const iglu::simdtypes::float2x2& value,
                                 size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float2x2), 1, arrayIndex);
}

void ShaderUniforms::setFloat2x2Array(const igl::NameHandle& uniformName,
                                      const iglu::simdtypes::float2x2* value,
                                      size_t count,
                                      size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float2x2), count, arrayIndex);
}

void ShaderUniforms::setFloat3x3(const igl::NameHandle& uniformName,
                                 const iglu::simdtypes::float3x3& value,
                                 size_t arrayIndex) {
  if (_backend == igl::BackendType::Metal || _backend == igl::BackendType::Vulkan) {
    setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float3x3), 1, arrayIndex);
  } else {
    // simdtypes::float3x3 has an extra float per float-vector.
    // Remove it so we can send the packed version to OpenGL
    float packedMatrix[9] = {0.0f};
    auto paddedMatrixPtr = reinterpret_cast<const float*>(&value);
    float* packedMatrixPtr = packedMatrix;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        *packedMatrixPtr++ = *paddedMatrixPtr++;
      }
      paddedMatrixPtr++; // padded float
    }
    setUniformBytes(uniformName, &packedMatrix, sizeof(packedMatrix), 1, arrayIndex);
  }
}

void ShaderUniforms::setFloat3x3Array(const igl::NameHandle& uniformName,
                                      const iglu::simdtypes::float3x3* value,
                                      size_t count,
                                      size_t arrayIndex) {
  if (_backend == igl::BackendType::Metal || _backend == igl::BackendType::Vulkan) {
    setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float3x3), count, arrayIndex);
  } else {
    // simdtypes::float3x3 has an extra float per float-vector.
    // Remove it so we can send the packed version to OpenGL
    auto paddedMatrixPtr = reinterpret_cast<const float*>(value);
    auto packedMatrix = new float[9 * count];
    auto packedMatrixPtr = packedMatrix;
    for (int n = 0; n < count; n++) {
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          *packedMatrixPtr++ = *paddedMatrixPtr++;
        }
        paddedMatrixPtr++; // skip over padded float
      }
    }
    setUniformBytes(uniformName, packedMatrix, sizeof(float) * 9 * count, 1, arrayIndex);
    delete[] packedMatrix;
  }
}

void ShaderUniforms::setFloat4x4(const igl::NameHandle& uniformName,
                                 const iglu::simdtypes::float4x4& value,
                                 size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::float4x4), 1, arrayIndex);
}

void ShaderUniforms::setFloat4x4Array(const igl::NameHandle& uniformName,
                                      const iglu::simdtypes::float4x4* value,
                                      size_t count,
                                      size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::float4x4), count, arrayIndex);
}

void ShaderUniforms::setInt(const igl::NameHandle& uniformName,
                            const iglu::simdtypes::int1& value,
                            size_t arrayIndex) {
  setUniformBytes(uniformName, &value, sizeof(iglu::simdtypes::int1), 1, arrayIndex);
}

void ShaderUniforms::setIntArray(const igl::NameHandle& uniformName,
                                 iglu::simdtypes::int1* value,
                                 size_t count,
                                 size_t arrayIndex) {
  setUniformBytes(uniformName, value, sizeof(iglu::simdtypes::int1), count, arrayIndex);
}

void ShaderUniforms::setBytes(const igl::NameHandle& bufferName,
                              void* data,
                              size_t size,
                              igl::ShaderStage stage,
                              IGL_MAYBE_UNUSED size_t arrayIndex) {
  IGL_ASSERT_MSG(arrayIndex == 0, "buffer arrays not supported");
  auto it = _allBuffersByName.find({bufferName, stage});
  if (it == _allBuffersByName.end()) {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid buffer name: %s\n", bufferName.toConstChar());
    return;
  }
  it->second->allocation->iglBuffer->upload(data, igl::BufferRange(size, 0));
}

void ShaderUniforms::setTexture(const std::string& name,
                                const std::shared_ptr<igl::ITexture>& value,
                                const std::shared_ptr<igl::ISamplerState>& sampler,
                                IGL_MAYBE_UNUSED size_t arrayIndex) {
  IGL_ASSERT_MSG(arrayIndex == 0, "texture arrays not supported");
  auto it = _allTexturesByName.find(name);
  if (it == _allTexturesByName.end()) {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid texture name: %s\n", name.c_str());
    return;
  }
  _allTexturesByName[name] = TextureSlot{value, value.get()};
  _allSamplersByName[name] = sampler;
}

void ShaderUniforms::setTexture(const std::string& name,
                                igl::ITexture* value,
                                const std::shared_ptr<igl::ISamplerState>& sampler) {
  auto it = _allTexturesByName.find(name);
  if (it == _allTexturesByName.end()) {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid texture name: %s\n", name.c_str());
    return;
  }
  _allTexturesByName[name] = TextureSlot{nullptr, value}; // non-owning
  _allSamplersByName[name] = sampler;
}

#if IGL_BACKEND_OPENGL
void ShaderUniforms::bindUniformOpenGL(const igl::NameHandle& uniformName,
                                       const UniformDesc& uniformDesc,
                                       const igl::IRenderPipelineState& pipelineState,
                                       igl::IRenderCommandEncoder& encoder) {
  const igl::BufferArgDesc::BufferMemberDesc& iglMemberDesc = uniformDesc.iglMemberDesc;
  igl::UniformDesc desc;
  desc.location = pipelineState.getIndexByName(uniformName, igl::ShaderStage::Fragment);
  desc.type = iglMemberDesc.type;
  desc.offset = iglMemberDesc.offset;
  desc.numElements = iglMemberDesc.arrayLength;
  desc.elementStride = igl::sizeForUniformType(iglMemberDesc.type);

  if (desc.location >= 0) {
    auto strongBuffer = uniformDesc.buffer.lock();
    if (strongBuffer) {
      // We are binding individual uniforms. Confirm that iglBuffer is null
      IGL_ASSERT(strongBuffer->allocation->iglBuffer == nullptr);
      encoder.bindUniform(desc, strongBuffer->allocation->ptr);
    }
  } else {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Uniform not found in shader: %s\n", uniformName.toConstChar());
  }
}
#endif

void ShaderUniforms::bindBuffer(igl::IDevice& device,
                                const igl::IRenderPipelineState& pipelineState,
                                igl::IRenderCommandEncoder& encoder,
                                BufferDesc* buffer) {
  if (!buffer) {
    return;
  }
  if (device.getBackendType() == igl::BackendType::OpenGL) {
#if IGL_BACKEND_OPENGL
    const auto& uniformName = buffer->iglBufferDesc.name;
    if (buffer->iglBufferDesc.isUniformBlock) {
      IGL_ASSERT(buffer->allocation->iglBuffer != nullptr);
      buffer->allocation->iglBuffer->upload(buffer->allocation->ptr,
                                            igl::BufferRange(buffer->allocation->size, 0));
      const auto& glPipelineState =
          static_cast<const igl::opengl::RenderPipelineState&>(pipelineState);
      encoder.bindBuffer(glPipelineState.getUniformBlockBindingPoint(uniformName),
                         bindTargetForShaderStage(buffer->iglBufferDesc.shaderStage),
                         buffer->allocation->iglBuffer,
                         0);
    } else {
      // not a uniform block
      IGL_ASSERT(buffer->iglBufferDesc.name == buffer->iglBufferDesc.members[0].name);
      IGL_ASSERT(buffer->uniforms.size() == 1);
      IGL_ASSERT(buffer->iglBufferDesc.name == buffer->uniforms[0].iglMemberDesc.name);
      auto& uniformDesc = buffer->uniforms[0];

      bindUniformOpenGL(uniformName, uniformDesc, pipelineState, encoder);
    }
#endif
  } else {
    if (buffer->allocation->iglBuffer) {
      uintptr_t subAllocatedOffset = 0;
      size_t uploadSize = buffer->allocation->size;
      if (buffer->isSuballocated && buffer->currentAllocation >= 0) {
        subAllocatedOffset = buffer->currentAllocation * buffer->suballocationsSize;
        uploadSize = buffer->suballocationsSize;
      }

      buffer->allocation->iglBuffer->upload((uint8_t*)buffer->allocation->ptr + subAllocatedOffset,
                                            igl::BufferRange(uploadSize, subAllocatedOffset));
      uint8_t bindTarget;
      if (device.getBackendType() == igl::BackendType::Vulkan) {
        bindTarget = igl::BindTarget::kAllGraphics;
      } else {
        bindTarget = bindTargetForShaderStage(buffer->iglBufferDesc.shaderStage);
      }
      encoder.bindBuffer(buffer->iglBufferDesc.bufferIndex,
                         bindTarget,
                         buffer->allocation->iglBuffer,
                         subAllocatedOffset);
    } else {
      encoder.bindBytes(buffer->iglBufferDesc.bufferIndex,
                        bindTargetForShaderStage(buffer->iglBufferDesc.shaderStage),
                        buffer->allocation->ptr,
                        buffer->iglBufferDesc.bufferDataSize);
    }
  }
}

// Bind the block which the specified uniform belongs to.
void ShaderUniforms::bind(igl::IDevice& device,
                          const igl::IRenderPipelineState& pipelineState,
                          igl::IRenderCommandEncoder& encoder,
                          const igl::NameHandle& uniformName) {
  auto range = _allUniformsByName.equal_range(uniformName);
  if (range.first == range.second) {
    IGL_LOG_ERROR_ONCE("[IGL][Error] Invalid uniform name: %s\n", uniformName.toConstChar());
    return;
  }

  for (auto it = range.first; it != range.second; ++it) {
    auto strongBuffer = it->second.buffer.lock();
    bindBuffer(device, pipelineState, encoder, strongBuffer.get());
  }
}

void ShaderUniforms::bind(igl::IDevice& device,
                          const igl::IRenderPipelineState& pipelineState,
                          igl::IRenderCommandEncoder& encoder) {
  for (auto& bufferDesc : _bufferDescs) {
    bindBuffer(device, pipelineState, encoder, bufferDesc.get());
  }

  for (auto& _textureDesc : _textureDescs) {
    auto textureIt = _allTexturesByName.find(_textureDesc.name);
    auto samplerIt = _allSamplersByName.find(_textureDesc.name);
    if (textureIt == _allTexturesByName.end() || samplerIt == _allSamplersByName.end()) {
      IGL_LOG_ERROR_ONCE("[IGL][Warning] No texture set for sampler: %s\n",
                         _textureDesc.name.c_str());
      continue;
    }
    encoder.bindTexture(_textureDesc.textureIndex,
                        bindTargetForShaderStage(_textureDesc.shaderStage),
                        textureIt->second.rawTexture ? textureIt->second.rawTexture
                                                     : textureIt->second.texture.get());

    // Assumption: each texture has an associated sampler at the same index in Metal
    encoder.bindSamplerState(_textureDesc.textureIndex,
                             bindTargetForShaderStage(_textureDesc.shaderStage),
                             samplerIt->second);
  }
}

igl::Result ShaderUniforms::setSuballocationIndex(const igl::NameHandle& name, int index) {
  if (_backend != igl::BackendType::Vulkan) {
    return igl::Result(igl::Result::Code::Unsupported,
                       "Suballocation is only available for Vulkan for now");
  }

  if (index < 0) {
    return igl::Result(igl::Result::Code::ArgumentOutOfRange,
                       "Invalid argument, index cannot be < 0");
  }

  auto range = _allUniformsByName.equal_range(name);
  if (range.first == range.second) {
    return igl::Result(igl::Result::Code::RuntimeError,
                       "Could not find uniform " + name.toString());
  }

  // At least one of the uniforms should be updated
  bool setIndexSuccess = false;

  for (auto it = range.first; it != range.second; ++it) {
    auto& uniformDesc = it->second;

    auto strongBuffer = uniformDesc.buffer.lock();

    if (!strongBuffer || !strongBuffer->isSuballocated) {
      continue;
    }

    // if index already exists, just update the allocation index
    if (std::find(strongBuffer->suballocations.begin(),
                  strongBuffer->suballocations.end(),
                  index) != strongBuffer->suballocations.end()) {
      strongBuffer->currentAllocation = index;
    } else {
      // Add new allocation
      // Make sure we have enough space
      auto currentSize = strongBuffer->suballocations.size() * strongBuffer->suballocationsSize;
      if (currentSize + strongBuffer->suballocationsSize > strongBuffer->allocation->size) {
        return igl::Result(igl::Result::Code::ArgumentOutOfRange,
                           "Cannot add new suballocation, exceeding buffer size of " +
                               std::to_string(strongBuffer->allocation->size));
      }

      strongBuffer->currentAllocation = index;
      strongBuffer->suballocations.push_back(index);
    }

    setIndexSuccess = true;
  }

  if (setIndexSuccess) {
    return igl::Result();
  } else {
    return igl::Result(igl::Result::Code::RuntimeError,
                       "Could not update suballocation index for " + name.toString());
  }
}
} // namespace iglu::material
