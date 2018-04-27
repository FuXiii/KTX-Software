/* -*- tab-width: 4; -*- */
/* vi: set sw=2 ts=4 expandtab: */

/*
 * ©2017 Mark Callow.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @internal
 * @class TextureArray
 * @~English
 *
 * @brief Test loading of 2D texture arrays.
 *
 * @author Mark Callow, www.edgewise-consulting.com.
 *
 * @par Acknowledgement
 * Thanks to Sascha Willems' - www.saschawillems.de - for the concept,
 * the VulkanTextOverlay class and the shaders used by this test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <algorithm>
#include <time.h> 
#include <vector>

#include <vulkan/vulkan.h>
#include <ktxvulkan.h>
#include "TextureArray.h"

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false

// Vertex layout for this example
struct TAVertex {
    float pos[3];
    float uv[2];
};

VulkanLoadTestSample*
TextureArray::create(VulkanContext& vkctx,
                 uint32_t width, uint32_t height,
                 const char* const szArgs, const std::string sBasePath)
{
    return new TextureArray(vkctx, width, height, szArgs, sBasePath);
}

TextureArray::TextureArray(VulkanContext& vkctx,
                 uint32_t width, uint32_t height,
                 const char* const szArgs, const std::string sBasePath)
        : VulkanLoadTestSample(vkctx, width, height, szArgs, sBasePath)
{
    zoom = -15.0f;
    rotationSpeed = 0.25f;
    rotation = { -15.0f, 35.0f, 0.0f };

    ktxVulkanDeviceInfo vdi;
    ktxVulkanDeviceInfo_Construct(&vdi, vkctx.gpu, vkctx.device,
                                  vkctx.queue, vkctx.commandPool, nullptr);
    KTX_error_code ktxresult;
    ktxTexture* kTexture;
    ktxresult =
           ktxTexture_CreateFromNamedFile((getAssetPath() + szArgs).c_str(),
                                           KTX_TEXTURE_CREATE_NO_FLAGS,
                                           &kTexture);
    if (KTX_SUCCESS != ktxresult) {
        std::stringstream message;
        
        message << "Creation of ktxTexture from \"" << getAssetPath() << szArgs
        << "\" failed: " << ktxErrorString(ktxresult);
        throw std::runtime_error(message.str());
    }
    ktxresult = ktxTexture_VkUpload(kTexture, &vdi, &textureArray);
    
    if (KTX_SUCCESS != ktxresult) {
        std::stringstream message;
        
        message << "ktxTexture_VkUpload failed: " << ktxErrorString(ktxresult);
        throw std::runtime_error(message.str());
    }
    
    // Checking if KVData contains keys of interest would go here.
    
    ktxTexture_Destroy(kTexture);
    ktxVulkanDeviceInfo_Destruct(&vdi);

    try {
        prepare();
    } catch (std::exception& e) {
        (void)e; // To quiet unused variable warnings from some compilers.
        cleanup();
        throw;
    }
}

TextureArray::~TextureArray()
{
    cleanup();
}

void
TextureArray::resize(uint32_t width, uint32_t height)
{
    this->w_width = width;
    this->w_height = height;
    vkctx.destroyDrawCommandBuffers();
    vkctx.createDrawCommandBuffers();
    buildCommandBuffers();
    updateUniformBufferMatrices();
}

void
TextureArray::run(uint32_t msTicks)
{
    // Nothing to do since the scene is not animated.
    // VulkanLoadTests base class redraws from the command buffer we built.
}

/* ------------------------------------------------------------------------- */

void
TextureArray::cleanup()
{
    // Clean up used Vulkan resources

    // Clean up texture resources
    if (sampler)
        vkctx.device.destroySampler(sampler);
    if (imageView)
        vkctx.device.destroyImageView(imageView);
    ktxVulkanTexture_Destruct(&textureArray, vkctx.device, nullptr);

    if (pipelines.solid)
        vkctx.device.destroyPipeline(pipelines.solid);
    if (pipelineLayout)
        vkctx.device.destroyPipelineLayout(pipelineLayout);
    if (descriptorSetLayout)
        vkctx.device.destroyDescriptorSetLayout(descriptorSetLayout);

    vkctx.destroyDrawCommandBuffers();
    quad.freeResources(vkctx.device);
    uniformDataVS.freeResources(vkctx.device);

    if (uboVS.instance != nullptr)
        delete[] uboVS.instance;
}

void
TextureArray::buildCommandBuffers()
{
    vk::CommandBufferBeginInfo cmdBufInfo({}, nullptr);

    vk::ClearValue clearValues[2];
    clearValues[0].color = defaultClearColor;
    clearValues[1].depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

    vk::RenderPassBeginInfo renderPassBeginInfo(vkctx.renderPass,
            nullptr,
            {{0, 0}, {w_width, w_height}},
            2,
            clearValues);

    for (uint32_t i = 0; i < vkctx.drawCmdBuffers.size(); ++i)
    {
        // Set target frame buffer
        renderPassBeginInfo.framebuffer = vkctx.framebuffers[i];

        VK_CHECK_RESULT(vkBeginCommandBuffer(vkctx.drawCmdBuffers[i],
                &static_cast<const VkCommandBufferBeginInfo&>(cmdBufInfo)));

        vkCmdBeginRenderPass(vkctx.drawCmdBuffers[i],
                &static_cast<const VkRenderPassBeginInfo&>(renderPassBeginInfo),
                VK_SUBPASS_CONTENTS_INLINE);

        vk::Viewport viewport(0, 0,
                              (float)w_width, (float)w_height,
                              0.0f, 1.0f);
        vkCmdSetViewport(vkctx.drawCmdBuffers[i], 0, 1,
                &static_cast<const VkViewport&>(viewport));

        vk::Rect2D scissor({0, 0}, {w_width, w_height});
        vkCmdSetScissor(vkctx.drawCmdBuffers[i], 0, 1,
                &static_cast<const VkRect2D&>(scissor));

        vkCmdBindDescriptorSets(vkctx.drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout, 0, 1,
                &static_cast<const VkDescriptorSet&>(descriptorSet), 0, NULL);

        VkDeviceSize offsets[1] = { 0 };
        vkCmdBindVertexBuffers(vkctx.drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1,
                &static_cast<const VkBuffer&>(quad.vertices.buf), offsets);
        vkCmdBindIndexBuffer(vkctx.drawCmdBuffers[i], quad.indices.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindPipeline(vkctx.drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.solid);

        vkCmdDrawIndexed(vkctx.drawCmdBuffers[i], quad.indexCount, textureArray.layerCount, 0, 0, 0);

        vkCmdEndRenderPass(vkctx.drawCmdBuffers[i]);

        VK_CHECK_RESULT(vkEndCommandBuffer(vkctx.drawCmdBuffers[i]));
    }
}

// Setup vertices for a single uv-mapped quad
void
TextureArray::generateQuad()
{
#define dim 2.5f
    std::vector<TAVertex> vertexBuffer =
    {
        { {  dim,  dim, 0.0f }, { 1.0f, 1.0f } },
        { { -dim,  dim, 0.0f }, { 0.0f, 1.0f } },
        { { -dim, -dim, 0.0f }, { 0.0f, 0.0f } },
        { {  dim, -dim, 0.0f }, { 1.0f, 0.0f } }
    };
#undef dim

    vkctx.createBuffer(
        vk::BufferUsageFlagBits::eVertexBuffer,
        vertexBuffer.size() * sizeof(TAVertex),
        vertexBuffer.data(),
        &quad.vertices.buf,
        &quad.vertices.mem);

    // Setup indices
    std::vector<uint32_t> indexBuffer = { 0,1,2, 2,3,0 };
    quad.indexCount = static_cast<uint32_t>(indexBuffer.size());

    vkctx.createBuffer(
        vk::BufferUsageFlagBits::eIndexBuffer,
        indexBuffer.size() * sizeof(uint32_t),
        indexBuffer.data(),
        &quad.indices.buf,
        &quad.indices.mem);
}

void
TextureArray::setupVertexDescriptions()
{
    // Binding description
    vertices.bindingDescriptions.resize(1);
    vertices.bindingDescriptions[0] =
        vk::VertexInputBindingDescription(
            VERTEX_BUFFER_BIND_ID,
            sizeof(TAVertex),
            vk::VertexInputRate::eVertex);

    // Attribute descriptions
    // Describes memory layout and shader positions
    vertices.attributeDescriptions.resize(2);
    // Location 0 : Position
    vertices.attributeDescriptions[0] =
        vk::VertexInputAttributeDescription(
            0,
            VERTEX_BUFFER_BIND_ID,
            vk::Format::eR32G32B32Sfloat,
            0);
    // Location 1 : Texture coordinates
    vertices.attributeDescriptions[1] =
        vk::VertexInputAttributeDescription(
            1,
            VERTEX_BUFFER_BIND_ID,
            vk::Format::eR32G32Sfloat,
            sizeof(float) * 3);

    vertices.inputState = vk::PipelineVertexInputStateCreateInfo();
    vertices.inputState.vertexBindingDescriptionCount =
                  static_cast<uint32_t>(vertices.bindingDescriptions.size());
    vertices.inputState.pVertexBindingDescriptions = vertices.bindingDescriptions.data();
    vertices.inputState.vertexAttributeDescriptionCount =
                  static_cast<uint32_t>(vertices.attributeDescriptions.size());
    vertices.inputState.pVertexAttributeDescriptions = vertices.attributeDescriptions.data();
}

void
TextureArray::setupDescriptorPool()
{
    // Example uses one ubo and one image sampler
    std::vector<vk::DescriptorPoolSize> poolSizes =
    {
        {vk::DescriptorType::eUniformBuffer, 1},
        {vk::DescriptorType::eCombinedImageSampler, 1}
    };

    vk::DescriptorPoolCreateInfo descriptorPoolInfo(
                                        {},
                                        2,
                                        static_cast<uint32_t>(poolSizes.size()),
                                        poolSizes.data());
    vkctx.device.createDescriptorPool(&descriptorPoolInfo, nullptr,
                                      &descriptorPool);
}

void
TextureArray::setupDescriptorSetLayout()
{
    std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings =
    {
        // Binding 0 : Vertex shader uniform buffer
        {0,
         vk::DescriptorType::eUniformBuffer,
         1,
         vk::ShaderStageFlagBits::eVertex},
        // Binding 1 : Fragment shader image sampler
        {1,
         vk::DescriptorType::eCombinedImageSampler,
         1,
         vk::ShaderStageFlagBits::eFragment},
    };

    vk::DescriptorSetLayoutCreateInfo descriptorLayout(
                              {},
                              static_cast<uint32_t>(setLayoutBindings.size()),
                              setLayoutBindings.data());

    vkctx.device.createDescriptorSetLayout(&descriptorLayout, nullptr,
                                           &descriptorSetLayout);

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo(
                                                    {},
                                                    1,
                                                    &descriptorSetLayout);

    vkctx.device.createPipelineLayout(&pipelineLayoutCreateInfo,
                                      nullptr,
                                      &pipelineLayout);
}

void
TextureArray::setupDescriptorSet()
{
    vk::DescriptorSetAllocateInfo allocInfo(
            descriptorPool,
            1,
            &descriptorSetLayout);

    vkctx.device.allocateDescriptorSets(&allocInfo, &descriptorSet);

    // Image descriptor for the color map texture
    vk::DescriptorImageInfo texArrayDescriptor(
            sampler,
            imageView,
            vk::ImageLayout::eGeneral);

    std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
    // Binding 0 : Vertex shader uniform buffer
    writeDescriptorSets.push_back(vk::WriteDescriptorSet(
            descriptorSet,
            0,
            0,
            1,
            vk::DescriptorType::eUniformBuffer,
            nullptr,
            &uniformDataVS.descriptor)
        );
    // Binding 1 : Fragment shader texture sampler
    writeDescriptorSets.push_back(vk::WriteDescriptorSet(
            descriptorSet,
            1,
            0,
            1,
            vk::DescriptorType::eCombinedImageSampler,
            &texArrayDescriptor)
    );

    vkctx.device.updateDescriptorSets(
                            static_cast<uint32_t>(writeDescriptorSets.size()),
                            writeDescriptorSets.data(),
                            0,
                            nullptr);
}

void
TextureArray::preparePipelines()
{
    vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState(
            {},
            vk::PrimitiveTopology::eTriangleList);

    vk::PipelineRasterizationStateCreateInfo rasterizationState;
    // Must be false because we haven't enabled the depthClamp device feature.
    rasterizationState.depthClampEnable = false;
    rasterizationState.rasterizerDiscardEnable = false;
    rasterizationState.polygonMode = vk::PolygonMode::eFill;
    rasterizationState.cullMode = vk::CullModeFlagBits::eNone;
    rasterizationState.frontFace = vk::FrontFace::eCounterClockwise;
    rasterizationState.lineWidth = 1.0f;

    vk::PipelineColorBlendAttachmentState blendAttachmentState;
    blendAttachmentState.blendEnable = false;
    //blendAttachmentState.colorWriteMask = 0xf;
    blendAttachmentState.colorWriteMask = vk::ColorComponentFlagBits::eR
                                          | vk::ColorComponentFlagBits::eG
                                          | vk::ColorComponentFlagBits::eB
                                          | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlendState;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachmentState;

    vk::PipelineDepthStencilStateCreateInfo depthStencilState;
    depthStencilState.depthTestEnable = true;
    depthStencilState.depthWriteEnable = true;
    depthStencilState.depthCompareOp = vk::CompareOp::eLessOrEqual;

    vk::PipelineViewportStateCreateInfo viewportState;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    vk::PipelineMultisampleStateCreateInfo multisampleState;
    multisampleState.rasterizationSamples = vk::SampleCountFlagBits::e1;

    std::vector<vk::DynamicState> dynamicStateEnables = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    vk::PipelineDynamicStateCreateInfo dynamicState(
            {},
            static_cast<uint32_t>(dynamicStateEnables.size()),
            dynamicStateEnables.data());

    // Load shaders
    std::array<vk::PipelineShaderStageCreateInfo,2> shaderStages;
    std::string filepath = getAssetPath() + "shaders/";
    shaderStages[0] = loadShader(filepath + "instancing.vert.spv",
                                vk::ShaderStageFlagBits::eVertex);
    shaderStages[1] = loadShader(filepath + "instancing.frag.spv",
                                vk::ShaderStageFlagBits::eFragment);

    vk::GraphicsPipelineCreateInfo pipelineCreateInfo;
    pipelineCreateInfo.layout = pipelineLayout;
    pipelineCreateInfo.renderPass = vkctx.renderPass;
    pipelineCreateInfo.pVertexInputState = &vertices.inputState;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineCreateInfo.pRasterizationState = &rasterizationState;
    pipelineCreateInfo.pColorBlendState = &colorBlendState;
    pipelineCreateInfo.pMultisampleState = &multisampleState;
    pipelineCreateInfo.pViewportState = &viewportState;
    pipelineCreateInfo.pDepthStencilState = &depthStencilState;
    pipelineCreateInfo.pDynamicState = &dynamicState;
    pipelineCreateInfo.stageCount = (uint32_t)shaderStages.size();
    pipelineCreateInfo.pStages = shaderStages.data();

    vkctx.device.createGraphicsPipelines(vkctx.pipelineCache, 1,
                                         &pipelineCreateInfo, nullptr,
                                         &pipelines.solid);
}

#define LAYERS_DECLARED_IN_SHADER 8U

void
TextureArray::prepareUniformBuffers()
{
    uboVS.instance = new UboInstanceData[textureArray.layerCount];

    uint32_t uboSize = sizeof(uboVS.matrices)
             + LAYERS_DECLARED_IN_SHADER * sizeof(UboInstanceData);

    // Vertex shader uniform buffer block
    vkctx.createBuffer(
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        uboSize,
        nullptr,
        &uniformDataVS.buffer,
        &uniformDataVS.memory,
        &uniformDataVS.descriptor);

    // Array indices and model matrices are fixed
    // Paren around std::min avoids a SNAFU that windef.h has a "min" macro.
    int32_t maxLayers = (std::min)(textureArray.layerCount, LAYERS_DECLARED_IN_SHADER);
    float offset = -1.5f;
    float center = (maxLayers * offset) / 2;
    for (int32_t i = 0; i < maxLayers; i++)
    {
        // Instance model matrix
        uboVS.instance[i].model = glm::translate(glm::mat4(), glm::vec3(0.0f, i * offset - center, 0.0f));
        uboVS.instance[i].model = glm::rotate(uboVS.instance[i].model, glm::radians(60.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        // Instance texture array index
        uboVS.instance[i].arrayIndex.x = (float)i;
    }

    // Update instanced part of the uniform buffer
    uint8_t *pData;
    uint32_t dataOffset = sizeof(uboVS.matrices);
    uint32_t dataSize = textureArray.layerCount * sizeof(UboInstanceData);
    VK_CHECK_RESULT(vkMapMemory(vkctx.device, uniformDataVS.memory, dataOffset, dataSize, 0, (void **)&pData));
    memcpy(pData, uboVS.instance, dataSize);
    vkUnmapMemory(vkctx.device, uniformDataVS.memory);

    updateUniformBufferMatrices();
}

void
TextureArray::updateUniformBufferMatrices()
{
    // Only updates the uniform buffer block part containing the global matrices

    // Projection
    uboVS.matrices.projection = glm::perspective(glm::radians(60.0f), (float)w_width / (float)w_height, 0.001f, 256.0f);

    // View
    uboVS.matrices.view = glm::translate(glm::mat4(), glm::vec3(0.0f, -1.0f, zoom));
    uboVS.matrices.view *= glm::translate(glm::mat4(), cameraPos);
    uboVS.matrices.view = glm::rotate(uboVS.matrices.view, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    uboVS.matrices.view = glm::rotate(uboVS.matrices.view, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    uboVS.matrices.view = glm::rotate(uboVS.matrices.view, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

    // Only update the matrices part of the uniform buffer
    uint8_t *pData;
    VK_CHECK_RESULT(vkMapMemory(vkctx.device, uniformDataVS.memory, 0, sizeof(uboVS.matrices), 0, (void **)&pData));
    memcpy(pData, &uboVS.matrices, sizeof(uboVS.matrices));
    vkUnmapMemory(vkctx.device, uniformDataVS.memory);
}

void
TextureArray::prepareSamplerAndView()
{
    // Create sampler.
    vk::SamplerCreateInfo samplerInfo;
    // Set the non-default values
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    samplerInfo.maxLod = (float)textureArray.levelCount;
    if (vkctx.gpuFeatures.samplerAnisotropy == VK_TRUE) {
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = 8;
    } else {
        // vulkan.hpp needs fixing
        samplerInfo.maxAnisotropy = 1.0;
    }
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueWhite;
    sampler = vkctx.device.createSampler(samplerInfo);
    
    // Create image view.
    // Textures are not directly accessed by the shaders and are abstracted
    // by image views containing additional information and sub resource
    // ranges.
    vk::ImageViewCreateInfo viewInfo;
    // Set the non-default values.
    viewInfo.image = textureArray.image;
    viewInfo.format = static_cast<vk::Format>(textureArray.imageFormat);
    viewInfo.viewType
    = static_cast<vk::ImageViewType>(textureArray.viewType);
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.layerCount = textureArray.layerCount;
    viewInfo.subresourceRange.levelCount = textureArray.levelCount;
    imageView = vkctx.device.createImageView(viewInfo);
}

void
TextureArray::prepare()
{
    prepareSamplerAndView();
    setupVertexDescriptions();
    generateQuad();
    prepareUniformBuffers();
    setupDescriptorSetLayout();
    preparePipelines();
    setupDescriptorPool();
    setupDescriptorSet();
    vkctx.createDrawCommandBuffers();
    buildCommandBuffers();
}


