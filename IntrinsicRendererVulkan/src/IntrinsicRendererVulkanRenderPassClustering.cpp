// Copyright 2016 Benjamin Glatzel
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Precompiled header file
#include "stdafx_vulkan.h"
#include "stdafx.h"

#define MAX_LIGHT_COUNT_PER_CLUSTER 256u
#define MAX_IRRAD_PROBES_PER_CLUSTER 4u
#define MAX_DECALS_PER_CLUSTER 64u

#define GRID_DEPTH_SLICE_COUNT 24u
#define GRID_SIZE_Y 8u

namespace Intrinsic
{
namespace Renderer
{
namespace Vulkan
{
namespace RenderPass
{
namespace
{
Resources::ImageRef _lightingBufferImageRef;
Resources::ImageRef _lightingBufferTransparentsImageRef;

Resources::FramebufferRef _framebufferLightingRef;
Resources::FramebufferRef _framebufferLightingTransparentsRef;
Resources::FramebufferRef _framebufferDecalsRef;

Resources::RenderPassRef _renderPassLightingRef;
Resources::RenderPassRef _renderPassDecalsRef;
Resources::PipelineRef _pipelineLightingRef;
Resources::PipelineRef _pipelineDecalsRef;

Resources::DrawCallRef _drawCallLightingRef;
Resources::DrawCallRef _drawCallLightingTransparentsRef;
Resources::DrawCallRef _drawCallDecalsRef;

Resources::BufferRef _lightBuffer;
Resources::BufferRef _lightIndexBuffer;
Resources::BufferRef _irradProbeBuffer;
Resources::BufferRef _irradProbeIndexBuffer;
Resources::BufferRef _decalBuffer;
Resources::BufferRef _decalIndexBuffer;

struct Light
{
  glm::vec4 posAndRadiusVS;
  glm::vec4 colorAndIntensity;
  glm::vec4 temp;
};

struct TestLight
{
  glm::vec3 spawnPos;
  Light light;
};

struct IrradProbe
{
  glm::vec4 posAndRadiusVS;
  glm::vec4 data0;
  glm::vec4 shData[7]; // Packed SH coefficients
};

struct Decal
{
  glm::mat4 viewProjMatrix;
  glm::vec4 posAndRadiusVS;
};

_INTR_ARRAY(TestLight) _testLights;

uint32_t _currentLightCount = 0u;
uint32_t _currentIrradProbeCount = 0u;
uint32_t _currentDecalCount = 0u;

uint32_t* _lightIndexBufferGpuMemory = nullptr;
Light* _lightBufferGpuMemory = nullptr;
Light* _lightBufferMemory = nullptr;
uint32_t* _irradProbeIndexBufferGpuMemory = nullptr;
IrradProbe* _irradProbeBufferGpuMemory = nullptr;
IrradProbe* _irradProbeBufferMemory = nullptr;
uint32_t* _decalIndexBufferGpuMemory = nullptr;
Decal* _decalBufferGpuMemory = nullptr;
Decal* _decalBufferMemory = nullptr;

// <-

struct LightingPerInstanceData
{
  glm::mat4 shadowViewProjMatrix[_INTR_MAX_SHADOW_MAP_COUNT];

  glm::vec4 nearFarWidthHeight;
  glm::vec4 nearFar;

  glm::vec4 data0;
} _lightingPerInstanceData;

struct DecalsPerInstanceData
{
  glm::vec4 nearFarWidthHeight;
  glm::vec4 nearFar;
} _decalsPerInstanceData;

// <-

// Have to match the values in the shader
const float _gridDepth = 10000.0f;
const glm::uvec3 _gridRes =
    glm::uvec3(16u, GRID_SIZE_Y, GRID_DEPTH_SLICE_COUNT);
const float _gridDepthExp = 3.0f;
const float _gridDepthSliceScale =
    _gridDepth / glm::pow((float)(_gridRes.z - 1u), _gridDepthExp);
const float _gridDepthSliceScaleRcp = 1.0f / _gridDepthSliceScale;
const uint32_t _totalClusterCount = _gridRes.x * _gridRes.y * _gridRes.z;

const uint32_t _totalLightGridSize =
    _totalClusterCount * MAX_LIGHT_COUNT_PER_CLUSTER;
const uint32_t _totalIrradGridSize =
    _totalClusterCount * MAX_IRRAD_PROBES_PER_CLUSTER;
const uint32_t _totalDecalGridSize =
    _totalClusterCount * MAX_DECALS_PER_CLUSTER;

// <-

_INTR_INLINE uint32_t calcClusterIndex(glm::uvec3 p_GridPos,
                                       uint32_t p_ClusterSize)
{
  return p_GridPos.x * p_ClusterSize +
         p_GridPos.y * _gridRes.x * p_ClusterSize +
         p_GridPos.z * _gridRes.y * _gridRes.x * p_ClusterSize;
}

_INTR_INLINE float calcGridDepthSlice(uint32_t p_DepthSliceIdx)
{
  return glm::pow((float)p_DepthSliceIdx, _gridDepthExp) * _gridDepthSliceScale;
}

_INTR_INLINE Math::AABB2 calcAABBForDepthSlice(uint32_t p_DepthSlice,
                                               glm::vec4 p_NearFarWidthHeight,
                                               glm::vec4 p_NearFar)
{
  const float gridStartDepth = calcGridDepthSlice(p_DepthSlice);
  const float gridEndDepth = calcGridDepthSlice(p_DepthSlice + 1u);

  const float gridDepth = gridEndDepth - gridStartDepth;
  const float gridHalfDepth = gridDepth * 0.5f;

  const float rayPos =
      (gridEndDepth - p_NearFar.x) / (p_NearFar.y - p_NearFar.x);

  const glm::vec2 gridHalfWidthHeight =
      glm::mix(glm::vec2(p_NearFarWidthHeight.x, p_NearFarWidthHeight.y),
               glm::vec2(p_NearFarWidthHeight.z, p_NearFarWidthHeight.w),
               rayPos) *
      0.5f;

  const glm::vec3 gridCenter =
      glm::vec3(0.0f, 0.0f, -gridStartDepth - gridHalfDepth);
  const glm::vec3 gridHalfExtent =
      glm::vec3(gridHalfWidthHeight.x, gridHalfWidthHeight.y, gridHalfDepth);

  return {gridCenter, gridHalfExtent};
}

_INTR_INLINE Math::AABB2 calcAABBForGridPos(glm::uvec3 p_GridPos,
                                            glm::vec4 p_NearFarWidthHeight,
                                            glm::vec4 p_NearFar)
{
  const float gridStartDepth = calcGridDepthSlice(p_GridPos.z);
  const float gridEndDepth = calcGridDepthSlice(p_GridPos.z + 1u);

  const float gridDepth = gridEndDepth - gridStartDepth;
  const float gridHalfDepth = gridDepth * 0.5f;

  const float rayPos =
      (gridEndDepth - p_NearFar.x) / (p_NearFar.y - p_NearFar.x);

  const glm::vec2 gridHalfWidthHeight =
      glm::mix(glm::vec2(p_NearFarWidthHeight.x, p_NearFarWidthHeight.y),
               glm::vec2(p_NearFarWidthHeight.z, p_NearFarWidthHeight.w),
               rayPos) *
      0.5f;

  const glm::vec3 gridCenter = glm::vec3(0.0f, 0.0f, gridStartDepth);
  const glm::vec3 gridHalfExtent =
      glm::vec3(gridHalfWidthHeight.x, gridHalfWidthHeight.y, gridHalfDepth);
  const glm::vec3 gridDimWithoutZ = glm::vec3(_gridRes.x, _gridRes.y, 1.0);

  const glm::vec3 aabbHalfExtent = gridHalfExtent / gridDimWithoutZ;
  glm::vec3 aabbCenter =
      gridCenter + aabbHalfExtent +
      (glm::vec3(p_GridPos) - gridDimWithoutZ * glm::vec3(0.5, 0.5, 0.0)) *
          aabbHalfExtent * glm::vec3(2.0, 2.0, 0.0);
  aabbCenter.z = -aabbCenter.z;

  return {aabbCenter, aabbHalfExtent};
}

struct CullingParallelTaskSet : enki::ITaskSet
{
  virtual ~CullingParallelTaskSet() = default;

  void ExecuteRange(enki::TaskSetPartition p_Range,
                    uint32_t p_ThreadNum) override
  {
    _INTR_PROFILE_CPU("Lighting", "Cull Lights And Probes For Depth Slice");

    uint32_t tempIrradIdxBuffer[MAX_IRRAD_PROBES_PER_CLUSTER];
    uint32_t tempLightIdxBuffer[MAX_LIGHT_COUNT_PER_CLUSTER];
    uint32_t tempDecalIdxBuffer[MAX_DECALS_PER_CLUSTER];

    for (uint32_t y = p_Range.start; y < p_Range.end; ++y)
    {
      for (uint32_t x = 0u; x < _gridRes.x; ++x)
      {
        const glm::uvec3 gridPos = glm::uvec3(x, y, _z);

        const Math::AABB2 clusterAABB = calcAABBForGridPos(
            gridPos, _lightingPerInstanceData.nearFarWidthHeight,
            _lightingPerInstanceData.nearFar);

        const uint32_t lightClusterIndex =
            calcClusterIndex(gridPos, MAX_LIGHT_COUNT_PER_CLUSTER);
        const uint32_t probeClusterIndex =
            calcClusterIndex(gridPos, MAX_IRRAD_PROBES_PER_CLUSTER);
        const uint32_t decalClusterIdx =
            calcClusterIndex(gridPos, MAX_DECALS_PER_CLUSTER);

        tempLightIdxBuffer[0] = 0u;
        for (uint32_t i = 0u;
             i < _availableLights.size() &&
             tempLightIdxBuffer[0] < MAX_LIGHT_COUNT_PER_CLUSTER - 1u;
             ++i)
        {
          uint32_t lidx = _availableLights[i];
          const Light& light = _lightBufferMemory[lidx];
          if (Math::calcIntersectSphereAABB(
                  {glm::vec3(light.posAndRadiusVS), light.posAndRadiusVS.w},
                  clusterAABB))
          {
            const uint32_t idx = tempLightIdxBuffer[0] + 1u;
            tempLightIdxBuffer[idx] = lidx;
            ++tempLightIdxBuffer[0];
          }
        }

        tempIrradIdxBuffer[0] = 0u;
        for (uint32_t i = 0u;
             i < _availableIrradProbes.size() &&
             tempIrradIdxBuffer[0] < MAX_IRRAD_PROBES_PER_CLUSTER - 1u;
             ++i)
        {
          uint32_t lidx = _availableIrradProbes[i];
          const IrradProbe& probe = _irradProbeBufferMemory[lidx];
          if (Math::calcIntersectSphereAABB(
                  {glm::vec3(probe.posAndRadiusVS), probe.posAndRadiusVS.w},
                  clusterAABB))
          {
            const uint32_t idx = tempIrradIdxBuffer[0] + 1u;
            tempIrradIdxBuffer[idx] = lidx;
            ++tempIrradIdxBuffer[0];
          }
        }

        tempDecalIdxBuffer[0] = 0u;
        for (uint32_t i = 0u;
             i < _availableDecals.size() &&
             tempDecalIdxBuffer[0] < MAX_DECALS_PER_CLUSTER - 1u;
             ++i)
        {
          uint32_t didx = _availableDecals[i];
          const Decal& decal = _decalBufferMemory[didx];

          if (Math::calcIntersectSphereAABB(
                  {glm::vec3(decal.posAndRadiusVS), decal.posAndRadiusVS.w},
                  clusterAABB))
          {
            const uint32_t idx = tempDecalIdxBuffer[0] + 1u;
            tempDecalIdxBuffer[idx] = didx;
            ++tempDecalIdxBuffer[0];
          }
        }

        memcpy(&_irradProbeIndexBufferGpuMemory[probeClusterIndex],
               tempIrradIdxBuffer,
               sizeof(uint32_t) * (tempIrradIdxBuffer[0] + 1u));
        memcpy(&_lightIndexBufferGpuMemory[lightClusterIndex],
               tempLightIdxBuffer,
               sizeof(uint32_t) * (tempLightIdxBuffer[0] + 1u));
        memcpy(&_decalIndexBufferGpuMemory[decalClusterIdx], tempDecalIdxBuffer,
               sizeof(uint32_t) * (tempDecalIdxBuffer[0] + 1u));
      }
    }
  }

  uint32_t _z;
  _INTR_ARRAY(uint32_t) _availableLights;
  _INTR_ARRAY(uint32_t) _availableIrradProbes;
  _INTR_ARRAY(uint32_t) _availableDecals;
} _cullingTaskSets[GRID_DEPTH_SLICE_COUNT];

_INTR_INLINE void cullAndWriteBuffers(Components::CameraRef p_CameraRef)
{
  _INTR_PROFILE_CPU("Clustered", "Cull And Write Buffers");

  // TODO: Add coarse frustum culling pre-pass
  {
    _INTR_PROFILE_CPU("Clustered", "Write Buffers");

    _currentLightCount = 0u;
    for (uint32_t i = 0u; i < Components::LightManager::_activeRefs.size(); ++i)
    {
      Components::LightRef lightRef = Components::LightManager::_activeRefs[i];
      Components::NodeRef lightNodeRef =
          Components::NodeManager::getComponentForEntity(
              Components::LightManager::_entity(lightRef));

      const glm::vec3 lightPosVS =
          Components::CameraManager::_viewMatrix(p_CameraRef) *
          glm::vec4(Components::NodeManager::_worldPosition(lightNodeRef), 1.0);
      _lightBufferMemory[_currentLightCount] = {
          glm::vec4(lightPosVS,
                    Components::LightManager::_descRadius(lightRef)),
          glm::vec4(Components::LightManager::_descColor(lightRef),
                    Components::LightManager::_descIntensity(lightRef)),
          glm::vec4(Components::LightManager::_descTemperature(lightRef))};
      ++_currentLightCount;
    }

    _currentDecalCount = 0u;
    for (uint32_t i = 0u; i < Components::DecalManager::_activeRefs.size(); ++i)
    {
      Components::DecalRef decalRef = Components::DecalManager::_activeRefs[i];
      Components::NodeRef decalNodeRef =
          Components::NodeManager::getComponentForEntity(
              Components::DecalManager::_entity(decalRef));

      const glm::vec3 decalHalfExtent =
          Components::DecalManager::_descHalfExtent(decalRef);
      const glm::vec3 decalWorldPos =
          Components::NodeManager::_worldPosition(decalNodeRef);
      const glm::quat decalWorldOrientation =
          Components::NodeManager::_worldOrientation(decalNodeRef);

      const glm::vec3 right =
          decalWorldOrientation * glm::vec3(decalHalfExtent.x, 0.0f, 0.0f);
      const glm::vec3 up =
          decalWorldOrientation * glm::vec3(0.0f, decalHalfExtent.y, 0.0f);
      const glm::vec3 forward =
          decalWorldOrientation *
          glm::vec3(0.0f, 0.0f, -decalHalfExtent.z * 2.0f);

      const Math::AABB decalAABB = Math::AABB(
          decalWorldPos - right - up, decalWorldPos + forward + right + up);

      const glm::mat4 decalViewMatrix =
          glm::lookAt(decalWorldPos, decalWorldPos + forward, up);
      const glm::mat4 decalProjectionMatrix =
          glm::ortho(-decalHalfExtent.x, decalHalfExtent.x, -decalHalfExtent.y,
                     decalHalfExtent.y, 0.01f, decalHalfExtent.z * 2.0f);

      Decal decal;
      {
        decal.posAndRadiusVS =
            Components::CameraManager::_viewMatrix(p_CameraRef) *
            glm::vec4(Math::calcAABBCenter(decalAABB), 1.0);
        decal.posAndRadiusVS.w =
            glm::length(Math::calcAABBHalfExtent(decalAABB));
        decal.viewProjMatrix =
            (decalProjectionMatrix * decalViewMatrix) *
            Components::CameraManager::_inverseViewMatrix(p_CameraRef);
      }
      _decalBufferMemory[_currentDecalCount] = decal;

      ++_currentDecalCount;
    }

    // Write test lights
    for (uint32_t i = 0u; i < _testLights.size(); ++i)
    {
      _lightBufferMemory[_currentLightCount] = _testLights[i].light;
      ++_currentLightCount;
    }

    // Sort probes by priority
    // TODO: Could be done once if a priority changes
    _currentIrradProbeCount = 0u;
    Components::IrradianceProbeManager::sortByPriority(
        Components::IrradianceProbeManager::_activeRefs);

    for (uint32_t i = 0u;
         i < Components::IrradianceProbeManager::_activeRefs.size(); ++i)
    {
      Components::IrradianceProbeRef irradProbeRef =
          Components::IrradianceProbeManager::_activeRefs[i];
      Components::NodeRef irradNodeRef =
          Components::NodeManager::getComponentForEntity(
              Components::IrradianceProbeManager::_entity(irradProbeRef));

      const glm::vec3 irradProbePosVS =
          Components::CameraManager::_viewMatrix(p_CameraRef) *
          glm::vec4(Components::NodeManager::_worldPosition(irradNodeRef), 1.0);
      _irradProbeBufferMemory[_currentIrradProbeCount].posAndRadiusVS =
          glm::vec4(
              irradProbePosVS,
              Components::IrradianceProbeManager::_descRadius(irradProbeRef));
      _irradProbeBufferMemory[_currentIrradProbeCount].data0 = glm::vec4(
          Components::IrradianceProbeManager::_descFalloffRangePerc(
              irradProbeRef),
          Components::IrradianceProbeManager::_descFalloffExp(irradProbeRef),
          0.0f, 0.0f);

      const _INTR_ARRAY(Irradiance::SH9)& shs =
          Components::IrradianceProbeManager::_descSHs(irradProbeRef);

      // Blend SHs according to the time of day
      Irradiance::SH9 blendedSH;
      if (!shs.empty())
      {
        if (shs.size() >= 2u)
        {
          const uint32_t leftIdx =
              std::min((uint32_t)(World::_currentTime * shs.size()),
                       (uint32_t)shs.size() - 2u);

          const float leftPerc = leftIdx / (float)shs.size();
          const float rightPerc = (leftIdx + 1u) / (float)shs.size();

          const float interp =
              (World::_currentTime - leftPerc) / (rightPerc - leftPerc);

          const Irradiance::SH9& left = shs[leftIdx];
          const Irradiance::SH9& right = shs[leftIdx + 1u];

          blendedSH = Irradiance::blend(left, right, interp);
        }
        else
        {
          blendedSH = shs[0];
        }
      }

      memcpy(_irradProbeBufferMemory[_currentIrradProbeCount].shData,
             &blendedSH, sizeof(Irradiance::SH9));
      ++_currentIrradProbeCount;
    }
  }

  // Find objects intersecting the depth slices and kick jobs for populated
  // ones
  _INTR_ARRAY(uint32_t) _activeTaskSets;
  _activeTaskSets.reserve(GRID_DEPTH_SLICE_COUNT);
  {
    _INTR_PROFILE_CPU("Lighting", "Find Slice And Kick Jobs");

    for (uint32_t z = 0u; z < _gridRes.z; ++z)
    {
      CullingParallelTaskSet& taskSet = _cullingTaskSets[z];
      taskSet._z = z;
      taskSet.m_SetSize = GRID_SIZE_Y;
      taskSet._availableLights.clear();
      taskSet._availableIrradProbes.clear();
      taskSet._availableDecals.clear();

      const Math::AABB2 depthSliceAABB =
          calcAABBForDepthSlice(z, _lightingPerInstanceData.nearFarWidthHeight,
                                _lightingPerInstanceData.nearFar);

      for (uint32_t i = 0u; i < _currentLightCount; ++i)
      {
        const Light& light = _lightBufferMemory[i];
        if (Math::calcIntersectSphereAABB(
                {glm::vec3(light.posAndRadiusVS), light.posAndRadiusVS.w},
                depthSliceAABB))
        {
          taskSet._availableLights.push_back(i);
        }
      }

      for (uint32_t i = 0u; i < _currentIrradProbeCount; ++i)
      {
        const IrradProbe& irradProbe = _irradProbeBufferMemory[i];
        if (Math::calcIntersectSphereAABB({glm::vec3(irradProbe.posAndRadiusVS),
                                           irradProbe.posAndRadiusVS.w},
                                          depthSliceAABB))
        {
          taskSet._availableIrradProbes.push_back(i);
        }
      }

      for (uint32_t i = 0u; i < _currentDecalCount; ++i)
      {
        const Decal& decal = _decalBufferMemory[i];

        if (Math::calcIntersectSphereAABB(
                {glm::vec3(decal.posAndRadiusVS), decal.posAndRadiusVS.w},
                depthSliceAABB))
        {
          taskSet._availableDecals.push_back(i);
        }
      }

      if (!taskSet._availableLights.empty() ||
          !taskSet._availableIrradProbes.empty() ||
          !taskSet._availableDecals.empty())
      {
        Application::_scheduler.AddTaskSetToPipe(&taskSet);
        _activeTaskSets.push_back(z);
      }
      else
      {
        // Reset counts
        for (uint32_t y = 0u; y < _gridRes.y; ++y)
        {
          for (uint32_t x = 0u; x < _gridRes.x; ++x)
          {
            const glm::uint32_t lightClusterIdx = calcClusterIndex(
                glm::uvec3(x, y, z), MAX_LIGHT_COUNT_PER_CLUSTER);
            const glm::uint32_t probeClusterIdx = calcClusterIndex(
                glm::uvec3(x, y, z), MAX_IRRAD_PROBES_PER_CLUSTER);
            const glm::uint32_t decalClusterIdx =
                calcClusterIndex(glm::uvec3(x, y, z), MAX_DECALS_PER_CLUSTER);

            _lightIndexBufferGpuMemory[lightClusterIdx] = 0u;
            _irradProbeIndexBufferGpuMemory[probeClusterIdx] = 0u;
            _decalIndexBufferGpuMemory[decalClusterIdx] = 0u;
          }
        }
      }
    }
  }

  memcpy(_lightBufferGpuMemory, _lightBufferMemory,
         _currentLightCount * sizeof(Light));
  memcpy(_irradProbeBufferGpuMemory, _irradProbeBufferMemory,
         _currentIrradProbeCount * sizeof(IrradProbe));
  memcpy(_decalBufferGpuMemory, _decalBufferMemory,
         _currentDecalCount * sizeof(Decal));

  {
    _INTR_PROFILE_CPU("Lighting", "Wait For Jobs");

    for (uint32_t i = 0u; i < _activeTaskSets.size(); ++i)
    {
      CullingParallelTaskSet& taskSet = _cullingTaskSets[_activeTaskSets[i]];
      Application::_scheduler.WaitforTaskSet(&taskSet);
    }
  }
}

// <-

_INTR_INLINE void renderLighting(Resources::FramebufferRef p_FramebufferRef,
                                 Resources::DrawCallRef p_DrawCall,
                                 Resources::ImageRef p_LightingBufferRef,
                                 Components::CameraRef p_CameraRef)
{
  using namespace Resources;

  // Update per instance data
  {
    // Post effect data
    _lightingPerInstanceData.data0.x = TaskManager::_totalTimePassed;
    _lightingPerInstanceData.data0.y = Clustering::_globalAmbientFactor;
    _lightingPerInstanceData.data0.z = World::_currentDayNightFactor;

    const _INTR_ARRAY(Core::Resources::FrustumRef)& shadowFrustums =
        RenderProcess::Default::_shadowFrustums[p_CameraRef];

    for (uint32_t i = 0u; i < shadowFrustums.size(); ++i)
    {
      Core::Resources::FrustumRef shadowFrustumRef = shadowFrustums[i];

      // Transform from camera view space => light proj. space
      _lightingPerInstanceData.shadowViewProjMatrix[i] =
          Core::Resources::FrustumManager::_viewProjectionMatrix(
              shadowFrustumRef) *
          Components::CameraManager::_inverseViewMatrix(p_CameraRef);
    }

    DrawCallRefArray dcsToUpdate = {p_DrawCall};
    DrawCallManager::allocateAndUpdateUniformMemory(
        dcsToUpdate, nullptr, 0u, &_lightingPerInstanceData,
        sizeof(LightingPerInstanceData));
  }

  VkCommandBuffer primaryCmdBuffer = RenderSystem::getPrimaryCommandBuffer();

  ImageManager::insertImageMemoryBarrier(
      p_LightingBufferRef, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  RenderSystem::beginRenderPass(_renderPassLightingRef, p_FramebufferRef,
                                VK_SUBPASS_CONTENTS_INLINE);
  {
    RenderSystem::dispatchDrawCall(p_DrawCall, primaryCmdBuffer);
  }
  RenderSystem::endRenderPass(_renderPassLightingRef);

  ImageManager::insertImageMemoryBarrier(
      p_LightingBufferRef, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// <-

_INTR_INLINE void renderDecals(Components::CameraRef p_CameraRef)
{
  using namespace Resources;

  // Update per instance data
  {
    _decalsPerInstanceData.nearFar = _lightingPerInstanceData.nearFar;
    _decalsPerInstanceData.nearFarWidthHeight =
        _lightingPerInstanceData.nearFarWidthHeight;

    DrawCallManager::allocateAndUpdateUniformMemory(
        {_drawCallDecalsRef}, nullptr, 0u, &_decalsPerInstanceData,
        sizeof(DecalsPerInstanceData));
  }

  VkCommandBuffer primaryCmdBuffer = RenderSystem::getPrimaryCommandBuffer();

  const ImageRef gbufferAlbedoRef =
      ImageManager::getResourceByName(_N(GBufferAlbedo));

  ImageManager::insertImageMemoryBarrier(
      gbufferAlbedoRef, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  RenderSystem::beginRenderPass(_renderPassDecalsRef, _framebufferDecalsRef,
                                VK_SUBPASS_CONTENTS_INLINE);
  {
    RenderSystem::dispatchDrawCall(_drawCallDecalsRef, primaryCmdBuffer);
  }
  RenderSystem::endRenderPass(_renderPassDecalsRef);

  ImageManager::insertImageMemoryBarrier(
      gbufferAlbedoRef, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
}

// <-

float Clustering::_globalAmbientFactor = 1.0f;

// <-

void Clustering::init()
{
  using namespace Resources;

  PipelineRefArray pipelinesToCreate;
  PipelineLayoutRefArray pipelineLayoutsToCreate;
  RenderPassRefArray renderpassesToCreate;

  // Pipeline layout
  PipelineLayoutRef plLighting;
  {
    plLighting = PipelineLayoutManager::createPipelineLayout(_N(Lighting));
    PipelineLayoutManager::resetToDefault(plLighting);

    GpuProgramManager::reflectPipelineLayout(
        8u, {Resources::GpuProgramManager::getResourceByName("lighting.frag")},
        plLighting);
  }
  pipelineLayoutsToCreate.push_back(plLighting);

  PipelineLayoutRef plDecals;
  {
    plDecals = PipelineLayoutManager::createPipelineLayout(_N(Decals));
    PipelineLayoutManager::resetToDefault(plDecals);

    GpuProgramManager::reflectPipelineLayout(
        8u, {Resources::GpuProgramManager::getResourceByName("decals.frag")},
        plDecals);
  }
  pipelineLayoutsToCreate.push_back(plDecals);

  // Render passes
  {
    _renderPassLightingRef = RenderPassManager::createRenderPass(_N(Lighting));
    RenderPassManager::resetToDefault(_renderPassLightingRef);

    AttachmentDescription colorAttachment = {Format::kB10G11R11UFloat, 0u};

    RenderPassManager::_descAttachments(_renderPassLightingRef)
        .push_back(colorAttachment);
  }
  renderpassesToCreate.push_back(_renderPassLightingRef);

  {
    _renderPassDecalsRef = RenderPassManager::createRenderPass(_N(Decals));
    RenderPassManager::resetToDefault(_renderPassDecalsRef);

    AttachmentDescription colorAttachment = {Format::kR16G16B16A16Float, 0u};

    RenderPassManager::_descAttachments(_renderPassDecalsRef)
        .push_back(colorAttachment);
  }
  renderpassesToCreate.push_back(_renderPassDecalsRef);

  // Pipelines
  {
    _pipelineLightingRef = PipelineManager::createPipeline(_N(Lighting));
    PipelineManager::resetToDefault(_pipelineLightingRef);

    PipelineManager::_descFragmentProgram(_pipelineLightingRef) =
        GpuProgramManager::getResourceByName("lighting.frag");
    PipelineManager::_descVertexProgram(_pipelineLightingRef) =
        GpuProgramManager::getResourceByName("fullscreen_triangle.vert");
    PipelineManager::_descRenderPass(_pipelineLightingRef) =
        _renderPassLightingRef;
    PipelineManager::_descPipelineLayout(_pipelineLightingRef) = plLighting;
    PipelineManager::_descVertexLayout(_pipelineLightingRef) = Dod::Ref();
    PipelineManager::_descDepthStencilState(_pipelineLightingRef) =
        DepthStencilStates::kDefaultNoDepthTestAndWrite;
  }

  {
    _pipelineDecalsRef = PipelineManager::createPipeline(_N(Decals));
    PipelineManager::resetToDefault(_pipelineDecalsRef);

    PipelineManager::_descFragmentProgram(_pipelineDecalsRef) =
        GpuProgramManager::getResourceByName("decals.frag");
    PipelineManager::_descVertexProgram(_pipelineDecalsRef) =
        GpuProgramManager::getResourceByName("fullscreen_triangle.vert");
    PipelineManager::_descRenderPass(_pipelineDecalsRef) = _renderPassDecalsRef;
    PipelineManager::_descPipelineLayout(_pipelineDecalsRef) = plDecals;
    PipelineManager::_descVertexLayout(_pipelineDecalsRef) = Dod::Ref();
    PipelineManager::_descDepthStencilState(_pipelineDecalsRef) =
        DepthStencilStates::kDefaultNoDepthTestAndWrite;

    PipelineManager::_descBlendStates(_pipelineDecalsRef).clear();
    PipelineManager::_descBlendStates(_pipelineDecalsRef)
        .push_back(BlendStates::kAlphaBlend);
  }
  pipelinesToCreate.push_back(_pipelineLightingRef);

  PipelineLayoutManager::createResources(pipelineLayoutsToCreate);
  RenderPassManager::createResources(renderpassesToCreate);
  PipelineManager::createResources(pipelinesToCreate);

  _INTR_ARRAY(Resources::BufferRef) buffersToCreate;

  // Buffers
  {
    _lightBuffer = BufferManager::createBuffer(_N(LightBuffer));
    {
      BufferManager::resetToDefault(_lightBuffer);
      BufferManager::addResourceFlags(
          _lightBuffer, Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_lightBuffer) = BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_lightBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_lightBuffer) =
          _INTR_MAX_LIGHT_COMPONENT_COUNT * sizeof(Light);
      buffersToCreate.push_back(_lightBuffer);
    }

    _lightIndexBuffer = BufferManager::createBuffer(_N(LightIndexBuffer));
    {
      BufferManager::resetToDefault(_lightIndexBuffer);
      BufferManager::addResourceFlags(
          _lightIndexBuffer, Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_lightIndexBuffer) = BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_lightIndexBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_lightIndexBuffer) =
          _totalLightGridSize * sizeof(uint32_t);
      buffersToCreate.push_back(_lightIndexBuffer);
    }

    _irradProbeBuffer = BufferManager::createBuffer(_N(IrradProbeBuffer));
    {
      BufferManager::resetToDefault(_irradProbeBuffer);
      BufferManager::addResourceFlags(
          _irradProbeBuffer, Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_irradProbeBuffer) = BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_irradProbeBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_irradProbeBuffer) =
          MAX_IRRAD_PROBES_PER_CLUSTER * _gridRes.x * _gridRes.y * _gridRes.z *
          sizeof(IrradProbe);
      buffersToCreate.push_back(_irradProbeBuffer);
    }

    _irradProbeIndexBuffer =
        BufferManager::createBuffer(_N(IrradProbeIndexBuffer));
    {
      BufferManager::resetToDefault(_irradProbeIndexBuffer);
      BufferManager::addResourceFlags(
          _irradProbeIndexBuffer,
          Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_irradProbeIndexBuffer) =
          BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_irradProbeIndexBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_irradProbeIndexBuffer) =
          _totalIrradGridSize * sizeof(uint32_t);
      buffersToCreate.push_back(_irradProbeIndexBuffer);
    }

    _decalBuffer = BufferManager::createBuffer(_N(DecalBuffer));
    {
      BufferManager::resetToDefault(_decalBuffer);
      BufferManager::addResourceFlags(
          _decalBuffer, Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_decalBuffer) = BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_decalBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_decalBuffer) =
          _INTR_MAX_DECAL_COMPONENT_COUNT * sizeof(Decal);
      buffersToCreate.push_back(_decalBuffer);
    }

    _decalIndexBuffer = BufferManager::createBuffer(_N(DecalIndexBuffer));
    {
      BufferManager::resetToDefault(_decalIndexBuffer);
      BufferManager::addResourceFlags(
          _decalIndexBuffer, Dod::Resources::ResourceFlags::kResourceVolatile);

      BufferManager::_descBufferType(_decalIndexBuffer) = BufferType::kStorage;
      BufferManager::_descMemoryPoolType(_decalIndexBuffer) =
          MemoryPoolType::kStaticStagingBuffers;
      BufferManager::_descSizeInBytes(_decalIndexBuffer) =
          _totalDecalGridSize * sizeof(uint32_t);
      buffersToCreate.push_back(_decalIndexBuffer);
    }

    Resources::BufferManager::createResources(buffersToCreate);

    {
      _lightBufferGpuMemory =
          (Light*)Resources::BufferManager::getGpuMemory(_lightBuffer);
      _lightIndexBufferGpuMemory =
          (uint32_t*)Resources::BufferManager::getGpuMemory(_lightIndexBuffer);

      _lightBufferMemory = (Light*)malloc(_totalLightGridSize * sizeof(Light));
    }

    {
      _irradProbeBufferGpuMemory =
          (IrradProbe*)Resources::BufferManager::getGpuMemory(
              _irradProbeBuffer);
      _irradProbeIndexBufferGpuMemory =
          (uint32_t*)Resources::BufferManager::getGpuMemory(
              _irradProbeIndexBuffer);

      _irradProbeBufferMemory =
          (IrradProbe*)malloc(_totalIrradGridSize * sizeof(IrradProbe));
    }

    {
      _decalBufferGpuMemory =
          (Decal*)Resources::BufferManager::getGpuMemory(_decalBuffer);
      _decalIndexBufferGpuMemory =
          (uint32_t*)Resources::BufferManager::getGpuMemory(_decalIndexBuffer);

      _decalBufferMemory = (Decal*)malloc(_totalDecalGridSize * sizeof(Decal));
    }
  }
}

// <-

namespace
{
_INTR_INLINE void setupLightingDrawCall(Resources::DrawCallRef p_DrawCallRef,
                                        bool p_Transparents)
{
  using namespace Resources;

  DrawCallManager::_descVertexCount(p_DrawCallRef) = 3u;

  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(PerInstance), GpuProgramType::kFragment,
      UniformManager::_perInstanceUniformBuffer, UboType::kPerInstanceFragment,
      sizeof(LightingPerInstanceData));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(PerFrame), GpuProgramType::kFragment,
      UniformManager::_perFrameUniformBuffer, UboType::kPerFrameFragment,
      sizeof(RenderProcess::PerFrameDataFrament));
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(albedoTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(
          !p_Transparents ? _N(GBufferAlbedo) : _N(GBufferTransparentsAlbedo)),
      Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(normalTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(
          !p_Transparents ? _N(GBufferNormal) : _N(GBufferTransparentsNormal)),
      Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(parameter0Tex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(!p_Transparents
                                          ? _N(GBufferParameter0)
                                          : _N(GBufferTransparentsParameter0)),
      Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(depthTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(
          !p_Transparents ? _N(GBufferDepth) : _N(GBufferTransparentsDepth)),
      Samplers::kNearestClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(ssaoTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(SSAO)), Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(kelvinLutTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(kelvin_rgb_LUT)),
      Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(specularTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(default_ibl_cube_specular)),
      Samplers::kLinearClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(noiseTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(noise)), Samplers::kLinearRepeat);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(shadowBufferTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(ShadowBuffer)), Samplers::kShadow);
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(MaterialBuffer), GpuProgramType::kFragment,
      MaterialBuffer::_materialBuffer, UboType::kInvalidUbo,
      BufferManager::_descSizeInBytes(MaterialBuffer::_materialBuffer));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(LightBuffer), GpuProgramType::kFragment, _lightBuffer,
      UboType::kInvalidUbo, BufferManager::_descSizeInBytes(_lightBuffer));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(LightIndexBuffer), GpuProgramType::kFragment,
      _lightIndexBuffer, UboType::kInvalidUbo,
      BufferManager::_descSizeInBytes(_lightIndexBuffer));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(IrradProbeBuffer), GpuProgramType::kFragment,
      _irradProbeBuffer, UboType::kInvalidUbo,
      BufferManager::_descSizeInBytes(_irradProbeBuffer));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(IrradProbeIndexBuffer), GpuProgramType::kFragment,
      _irradProbeIndexBuffer, UboType::kInvalidUbo,
      BufferManager::_descSizeInBytes(_irradProbeIndexBuffer));
}

_INTR_INLINE void setupDecalsDrawCall(Resources::DrawCallRef p_DrawCallRef)
{
  using namespace Resources;

  DrawCallManager::_descVertexCount(p_DrawCallRef) = 3u;

  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(PerInstance), GpuProgramType::kFragment,
      UniformManager::_perInstanceUniformBuffer, UboType::kPerInstanceFragment,
      sizeof(DecalsPerInstanceData));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(PerFrame), GpuProgramType::kFragment,
      UniformManager::_perFrameUniformBuffer, UboType::kPerFrameFragment,
      sizeof(RenderProcess::PerFrameDataFrament));
  DrawCallManager::bindImage(p_DrawCallRef, _N(depthTex),
                             GpuProgramType::kFragment,
                             ImageManager::getResourceByName(_N(GBufferDepth)),
                             Samplers::kNearestClamp);
  DrawCallManager::bindImage(
      p_DrawCallRef, _N(testTex), GpuProgramType::kFragment,
      ImageManager::getResourceByName(_N(foam)), Samplers::kLinearClamp);

  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(DecalBuffer), GpuProgramType::kFragment, _decalBuffer,
      UboType::kInvalidUbo, BufferManager::_descSizeInBytes(_decalBuffer));
  DrawCallManager::bindBuffer(
      p_DrawCallRef, _N(DecalIndexBuffer), GpuProgramType::kFragment,
      _decalIndexBuffer, UboType::kInvalidUbo,
      BufferManager::_descSizeInBytes(_decalIndexBuffer));
}
}

void Clustering::onReinitRendering()
{
  using namespace Resources;

  ImageRefArray imgsToDestroy;
  ImageRefArray imgsToCreate;
  FramebufferRefArray framebuffersToDestroy;
  FramebufferRefArray framebuffersToCreate;
  DrawCallRefArray drawCallsToDestroy;
  DrawCallRefArray drawcallsToCreate;

  // Cleanup old resources
  {
    if (_drawCallLightingRef.isValid())
      drawCallsToDestroy.push_back(_drawCallLightingRef);
    if (_drawCallLightingTransparentsRef.isValid())
      drawCallsToDestroy.push_back(_drawCallLightingTransparentsRef);
    if (_drawCallDecalsRef.isValid())
      drawCallsToDestroy.push_back(_drawCallDecalsRef);

    if (_framebufferLightingRef.isValid())
      framebuffersToDestroy.push_back(_framebufferLightingRef);
    if (_framebufferLightingTransparentsRef.isValid())
      framebuffersToDestroy.push_back(_framebufferLightingTransparentsRef);
    if (_framebufferDecalsRef.isValid())
      framebuffersToDestroy.push_back(_framebufferDecalsRef);

    if (_lightingBufferImageRef.isValid())
      imgsToDestroy.push_back(_lightingBufferImageRef);
    if (_lightingBufferTransparentsImageRef.isValid())
      imgsToDestroy.push_back(_lightingBufferTransparentsImageRef);

    FramebufferManager::destroyFramebuffersAndResources(framebuffersToDestroy);
    DrawCallManager::destroyDrawCallsAndResources(drawCallsToDestroy);
    ImageManager::destroyImagesAndResources(imgsToDestroy);
  }

  glm::uvec3 dim = glm::uvec3(RenderSystem::_backbufferDimensions.x,
                              RenderSystem::_backbufferDimensions.y, 1u);

  _lightingBufferImageRef = ImageManager::createImage(_N(LightBuffer));
  {
    ImageManager::resetToDefault(_lightingBufferImageRef);
    ImageManager::addResourceFlags(
        _lightingBufferImageRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);
    ImageManager::_descMemoryPoolType(_lightingBufferImageRef) =
        MemoryPoolType::kResolutionDependentImages;

    ImageManager::_descDimensions(_lightingBufferImageRef) = dim;
    ImageManager::_descImageFormat(_lightingBufferImageRef) =
        Format::kB10G11R11UFloat;
    ImageManager::_descImageType(_lightingBufferImageRef) = ImageType::kTexture;
  }
  imgsToCreate.push_back(_lightingBufferImageRef);

  _lightingBufferTransparentsImageRef =
      ImageManager::createImage(_N(LightBufferTransparents));
  {
    ImageManager::resetToDefault(_lightingBufferTransparentsImageRef);
    ImageManager::addResourceFlags(
        _lightingBufferTransparentsImageRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);
    ImageManager::_descMemoryPoolType(_lightingBufferTransparentsImageRef) =
        MemoryPoolType::kResolutionDependentImages;

    ImageManager::_descDimensions(_lightingBufferTransparentsImageRef) = dim;
    ImageManager::_descImageFormat(_lightingBufferTransparentsImageRef) =
        Format::kB10G11R11UFloat;
    ImageManager::_descImageType(_lightingBufferTransparentsImageRef) =
        ImageType::kTexture;
  }
  imgsToCreate.push_back(_lightingBufferTransparentsImageRef);

  ImageManager::createResources(imgsToCreate);

  _framebufferLightingRef = FramebufferManager::createFramebuffer(_N(Lighting));
  {
    FramebufferManager::resetToDefault(_framebufferLightingRef);
    FramebufferManager::addResourceFlags(
        _framebufferLightingRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);

    FramebufferManager::_descAttachedImages(_framebufferLightingRef)
        .push_back(_lightingBufferImageRef);

    FramebufferManager::_descDimensions(_framebufferLightingRef) =
        glm::uvec2(RenderSystem::_backbufferDimensions.x,
                   RenderSystem::_backbufferDimensions.y);
    FramebufferManager::_descRenderPass(_framebufferLightingRef) =
        _renderPassLightingRef;

    framebuffersToCreate.push_back(_framebufferLightingRef);
  }

  _framebufferLightingTransparentsRef =
      FramebufferManager::createFramebuffer(_N(LightingTransparents));
  {
    FramebufferManager::resetToDefault(_framebufferLightingTransparentsRef);
    FramebufferManager::addResourceFlags(
        _framebufferLightingTransparentsRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);

    FramebufferManager::_descAttachedImages(_framebufferLightingTransparentsRef)
        .push_back(_lightingBufferTransparentsImageRef);

    FramebufferManager::_descDimensions(_framebufferLightingTransparentsRef) =
        glm::uvec2(RenderSystem::_backbufferDimensions.x,
                   RenderSystem::_backbufferDimensions.y);
    FramebufferManager::_descRenderPass(_framebufferLightingTransparentsRef) =
        _renderPassLightingRef;

    framebuffersToCreate.push_back(_framebufferLightingTransparentsRef);
  }

  _framebufferDecalsRef = FramebufferManager::createFramebuffer(_N(Decals));
  {
    FramebufferManager::resetToDefault(_framebufferDecalsRef);
    FramebufferManager::addResourceFlags(
        _framebufferDecalsRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);

    FramebufferManager::_descAttachedImages(_framebufferDecalsRef)
        .push_back(ImageManager::getResourceByName(_N(GBufferAlbedo)));

    FramebufferManager::_descDimensions(_framebufferDecalsRef) =
        glm::uvec2(RenderSystem::_backbufferDimensions.x,
                   RenderSystem::_backbufferDimensions.y);
    FramebufferManager::_descRenderPass(_framebufferDecalsRef) =
        _renderPassDecalsRef;

    framebuffersToCreate.push_back(_framebufferDecalsRef);
  }

  FramebufferManager::createResources(framebuffersToCreate);

  // Draw calls
  _drawCallLightingRef = DrawCallManager::createDrawCall(_N(Lighting));
  {
    DrawCallManager::resetToDefault(_drawCallLightingRef);
    DrawCallManager::addResourceFlags(
        _drawCallLightingRef, Dod::Resources::ResourceFlags::kResourceVolatile);

    DrawCallManager::_descPipeline(_drawCallLightingRef) = _pipelineLightingRef;
    setupLightingDrawCall(_drawCallLightingRef, false);
  }

  drawcallsToCreate.push_back(_drawCallLightingRef);

  _drawCallLightingTransparentsRef =
      DrawCallManager::createDrawCall(_N(LightingTransparents));
  {
    DrawCallManager::resetToDefault(_drawCallLightingTransparentsRef);
    DrawCallManager::addResourceFlags(
        _drawCallLightingTransparentsRef,
        Dod::Resources::ResourceFlags::kResourceVolatile);

    DrawCallManager::_descPipeline(_drawCallLightingTransparentsRef) =
        _pipelineLightingRef;
    setupLightingDrawCall(_drawCallLightingTransparentsRef, true);
  }
  drawcallsToCreate.push_back(_drawCallLightingTransparentsRef);

  _drawCallDecalsRef = DrawCallManager::createDrawCall(_N(Decals));
  {
    DrawCallManager::resetToDefault(_drawCallDecalsRef);
    DrawCallManager::addResourceFlags(
        _drawCallDecalsRef, Dod::Resources::ResourceFlags::kResourceVolatile);

    DrawCallManager::_descPipeline(_drawCallDecalsRef) = _pipelineDecalsRef;
    setupDecalsDrawCall(_drawCallDecalsRef);
  }
  drawcallsToCreate.push_back(_drawCallDecalsRef);

  DrawCallManager::createResources(drawcallsToCreate);
}

// <-

void Clustering::destroy() {}

// <-

namespace
{
void spawnAndSimulateTestLights(Components::CameraRef p_CameraRef)
{
  // Spawn lights if none are there yet
  if (_testLights.empty())
  {
    static uint32_t testLightCount = 4096u * 4u;

    _testLights.resize(testLightCount);
    for (uint32_t i = 0u; i < testLightCount; ++i)
    {
      TestLight& light = _testLights[i];
      light.spawnPos =
          glm::vec3(Math::calcRandomFloatMinMax(-2000.0f, 2000.0f), 0.0f,
                    Math::calcRandomFloatMinMax(-2000.0f, 2000.0f));
      light.light.colorAndIntensity =
          glm::vec4(Math::calcRandomFloat(), Math::calcRandomFloat(),
                    Math::calcRandomFloat(), 5000.0f);
      light.light.temp = glm::vec4(6500.0f);
      light.light.posAndRadiusVS = glm::vec4(glm::vec3(0.0f), 100.0f);
    }
  }

  // Update position and lights
  for (uint32_t i = 0u; i < _testLights.size(); ++i)
  {
    TestLight& light = _testLights[i];
    const glm::vec3 worldPos = glm::vec3(
        light.spawnPos.x,
        light.spawnPos.y + 2000.0f * sin(light.spawnPos.x + light.spawnPos.y +
                                         TaskManager::_totalTimePassed * 0.1f),
        light.spawnPos.z);

    light.light.posAndRadiusVS = glm::vec4(
        glm::vec3(Components::CameraManager::_viewMatrix(p_CameraRef) *
                  glm::vec4(worldPos, 1.0f)),
        light.light.posAndRadiusVS.w);
  }
}
}

void Clustering::render(float p_DeltaT, Components::CameraRef p_CameraRef)
{
  _INTR_PROFILE_CPU("Render Pass", "Render Clustering");
  _INTR_PROFILE_GPU("Render Clustering");

  // Testing code for profiling purposes
  {
    Components::NodeRef rootNodeRef = World::getRootNode();
    Entity::EntityRef rootEntityRef =
        Components::NodeManager::_entity(rootNodeRef);

    if (Entity::EntityManager::_name(rootEntityRef) == _N(LightingTest))
    {
      spawnAndSimulateTestLights(p_CameraRef);
    }
    else
    {
      _testLights.clear();
    }
  }

  // Update global per instance data
  {
    _lightingPerInstanceData.nearFar = glm::vec4(
        Components::CameraManager::_descNearPlane(p_CameraRef),
        Components::CameraManager::_descFarPlane(p_CameraRef), 0.0f, 0.0f);

    Math::FrustumCorners viewSpaceCorners;
    Math::extractFrustumsCorners(
        Components::CameraManager::_inverseProjectionMatrix(p_CameraRef),
        viewSpaceCorners);

    _lightingPerInstanceData.nearFarWidthHeight = glm::vec4(
        viewSpaceCorners.c[3].x - viewSpaceCorners.c[2].x /* Near Width */,
        viewSpaceCorners.c[2].y - viewSpaceCorners.c[1].y /* Near Height */,
        viewSpaceCorners.c[7].x - viewSpaceCorners.c[6].x /* Far Width */,
        viewSpaceCorners.c[6].y - viewSpaceCorners.c[5].y /* Far Height */);
  }

  cullAndWriteBuffers(p_CameraRef);

  {
    _INTR_PROFILE_GPU("Decals");

    renderDecals(p_CameraRef);
  }

  {
    _INTR_PROFILE_GPU("Opaque Lighting");

    renderLighting(_framebufferLightingRef, _drawCallLightingRef,
                   _lightingBufferImageRef, p_CameraRef);
  }

  {
    _INTR_PROFILE_GPU("Transparent Lighting");

    renderLighting(_framebufferLightingTransparentsRef,
                   _drawCallLightingTransparentsRef,
                   _lightingBufferTransparentsImageRef, p_CameraRef);
  }
}
}
}
}
}