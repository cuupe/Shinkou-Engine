#include "shinkou/editor/EditorModelPreviewRenderer.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/platform/Window.h"
#include "shinkou/ui/Render.h"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

using namespace shinkou;
using namespace shinkou::editor;

int main(int argc, char** argv) {
    const std::string backendName = argc > 1 ? argv[1] : "dx11";
    const auto backend = backendName == "dx12" ? render::BackendApi::DirectX12 :
        backendName == "vulkan" ? render::BackendApi::Vulkan : render::BackendApi::DirectX11;
    const bool requireNative = argc > 2 && std::string(argv[2]) == "--require-native";
    auto vertices = std::make_shared<std::vector<math::Vec3>>();
    vertices->push_back({-1.0f, -1.0f, 0.0f});
    vertices->push_back({1.0f, -1.0f, 0.0f});
    vertices->push_back({0.0f, 1.0f, 0.0f});
    auto indices = std::make_shared<std::vector<std::uint32_t>>();
    indices->insert(indices->end(), {0u, 1u, 2u});
    auto textureCoordinates = std::make_shared<std::vector<math::Vec2>>();
    textureCoordinates->insert(textureCoordinates->end(), {{0.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f}});
    auto normals = std::make_shared<std::vector<math::Vec3>>();
    normals->insert(normals->end(), {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}});
    auto materials = std::make_shared<std::vector<EditorModelMaterialPreview>>();
    EditorModelMaterialPreview material;
    material.baseColorFactor = {0.2f, 0.4f, 0.8f, 1.0f};
    material.metallicFactor = 0.25f;
    material.roughnessFactor = 0.75f;
    material.metallicRoughnessTexture = 0;
    materials->push_back(material);
    auto wire = std::make_shared<std::vector<ui::Vec2>>();
    wire->insert(wire->end(), {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f},
                               {0.5f, 0.0f}, {0.0f, 1.0f}});
    auto snapshot = std::make_shared<EditorModelPreviewSnapshot>();
    snapshot->revision = 7;
    snapshot->vertexCount = vertices->size();
    snapshot->triangleCount = 1;
    snapshot->minX = -1.0f; snapshot->maxX = 1.0f;
    snapshot->minY = -1.0f; snapshot->maxY = 1.0f;
    snapshot->minZ = 0.0f; snapshot->maxZ = 0.0f;
    snapshot->vertices = vertices;
    snapshot->indices = indices;
    snapshot->textureCoordinates = textureCoordinates;
    snapshot->normals = normals;
    snapshot->materials = materials;
    snapshot->wireSegments = wire;
    assert(snapshot->valid());

    EditorModelPreviewScene scene;
    scene.set_snapshot(snapshot);
    const auto sceneState = scene.state();
    assert(sceneState && sceneState->valid());

    render::Renderer renderer(render::BackendApi::Null);
    assert(renderer.initialize());
    renderer.begin_graph();
    const auto color = renderer.graph().create_texture({64, 64, 1, 1, "rgba8", true, false, {}});
    renderer.graph().add_pass("fixture_color", {{color, render::ResourceUsage::ColorAttachment0}},
                              [](auto&, const auto&) {});

    EditorModelPreviewRenderer previewRenderer;
    const auto state = previewRenderer.render(renderer, *snapshot, *sceneState, -1, {});
    assert(state.backend == render::BackendApi::Null);
    assert(!state.rendererReady);
    assert(!state.geometryUploaded);
    assert(state.status.find("Null") != std::string::npos);
    assert(renderer.graph().passes().size() == 1);
    previewRenderer.clear(renderer);

    EditorModelPreviewSnapshot invalid = *snapshot;
    invalid.indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0u, 1u});
    const auto invalidState = previewRenderer.render(renderer, invalid, *sceneState, -1, {});
    assert(!invalidState.rendererReady);
    assert(invalidState.status.find("invalid") != std::string::npos);

    // A native D3D11 device may be unavailable in CI or on a headless host.
    // When it is available, exercise shader/pipeline/material/texture
    // creation without claiming that a window pixel was presented.
    bool nativePathAttempted = false;
    bool nativePathExercised = false;
    bool nativeMaterialFactorsExercised = false;
    bool nativeTextureExercised = false;
    bool nativeMetallicRoughnessTextureExercised = false;
    bool nativeOffscreenExecutionExercised = false;
    bool nativeOffscreenReadbackExercised = false;
    bool nativeOffscreenPixelActivityExercised = false;
    bool nativeAnimatedUpdateExercised = false;
    bool nativeAnimatedReadbackExercised = false;
    bool nativeDepthTargetExercised = false;
    bool nativeTextureReadbackExercised = false;
    platform::Window nativeWindow;
    const bool windowReady = nativeWindow.create({"Model Preview GPU QA", 640, 480, false});
    render::RenderBackendConfig backendConfig{nativeWindow.native_handle(), 640, 480, false};
    backendConfig.enableValidation = requireNative;
    {
    render::Renderer nativeRenderer(backend, backendConfig);
    if (requireNative) assert(windowReady);
    if (nativeRenderer.initialize()) {
        const auto capabilities = nativeRenderer.capabilities();
        if (capabilities.deviceReady && capabilities.supportsEditorViewportScissor) {
            nativePathAttempted = true;
            nativeRenderer.begin_graph();
            nativeRenderer.begin_editor_frame();
            render::EditorViewportSeam seam;
            seam.viewport = {0.0f, 0.0f, 64.0f, 64.0f};
            seam.enabled = true;
            seam.strictTarget = true;
            if (nativeRenderer.set_editor_viewport(seam)) {
                const auto nativeState = previewRenderer.render(nativeRenderer, *snapshot, *sceneState, 0, {});
                if (!nativeState.rendererReady) {
                    std::cerr << "native preview failure: " << nativeState.status
                              << " renderer-error=" << nativeRenderer.last_error() << "\n";
                }
                if (nativeState.rendererReady) {
                    nativePathExercised = true;
                    assert(nativeState.geometryUploaded);
                    assert(nativeState.materialApplied);
                    assert(nativeState.materialFactorsApplied);
                    assert(nativeState.offscreenTargetReady);
                    assert(nativeState.depthTargetReady);
                    assert(nativeState.offscreenCompositeApplied);
                    assert(nativeState.status.find("offscreen target composited") != std::string::npos);
                    assert(nativeState.status.find("depth-tested") != std::string::npos);
                    nativeMaterialFactorsExercised = true;
                    nativeDepthTargetExercised = true;
                    assert(nativeState.textureCoordinatesApplied);
                    assert(nativeState.normalsApplied);
                    assert(!nativeState.textureSampled);
                    assert(nativeRenderer.graph().passes().size() == 2);
                    auto image = std::make_shared<ui::UiImageSnapshot>();
                    image->revision = 11;
                    image->width = 1;
                    image->height = 1;
                    image->bgraPremultiplied = {32u, 96u, 192u, 255u};
                    const auto texturedState = previewRenderer.render(nativeRenderer, *snapshot, *sceneState,
                                                                        0, image);
                    nativeTextureExercised = texturedState.rendererReady && texturedState.textureSampled;
                    assert(nativeTextureExercised);
                    assert(texturedState.textureRoleApplied);
                    assert(texturedState.offscreenTargetReady);
                    assert(texturedState.depthTargetReady);
                    assert(texturedState.offscreenCompositeApplied);
                    assert(texturedState.baseColorTextureSampled);
                    assert(!texturedState.normalTextureSampled);
                    assert(nativeRenderer.graph().passes().size() == 4);
                    const auto normalState = previewRenderer.render(
                        nativeRenderer, *snapshot, *sceneState, 0, image,
                        EditorModelPreviewTextureRole::Normal);
                    assert(normalState.rendererReady);
                    assert(normalState.textureRoleApplied);
                    assert(!normalState.baseColorTextureSampled);
                    assert(normalState.normalTextureSampled);
                    assert(normalState.textureSampled);
                    assert(normalState.status.find("normal texture sampled") != std::string::npos);
                    assert(nativeRenderer.graph().passes().size() == 6);
                    const auto metallicRoughnessState = previewRenderer.render(
                        nativeRenderer, *snapshot, *sceneState, 0, image,
                        EditorModelPreviewTextureRole::MetallicRoughness);
                    assert(metallicRoughnessState.rendererReady);
                    assert(metallicRoughnessState.textureRoleApplied);
                    assert(!metallicRoughnessState.baseColorTextureSampled);
                    assert(!metallicRoughnessState.normalTextureSampled);
                    assert(metallicRoughnessState.metallicRoughnessTextureSampled);
                    assert(metallicRoughnessState.textureSampled);
                    assert(metallicRoughnessState.status.find("metallic/roughness texture sampled") !=
                           std::string::npos);
                    nativeMetallicRoughnessTextureExercised = true;
                    assert(nativeRenderer.graph().passes().size() == 8);

                    // Change only the retained scene pose and animation revision.
                    // The renderer must upload the transformed vertices again,
                    // while preserving the same model/material contract.
                    auto animatedState = std::make_shared<EditorModelPreviewSceneState>(*sceneState);
                    auto animatedVertices = std::make_shared<std::vector<math::Vec3>>();
                    animatedVertices->insert(animatedVertices->end(), {
                        {-0.5f, -1.0f, 0.0f}, {1.0f, -0.5f, 0.0f}, {-0.5f, 1.0f, 0.0f}});
                    animatedState->transformedVertices = animatedVertices;
                    animatedState->animationRevision = 1;
                    animatedState->projectionRevision += 1;
                    const auto animatedStateResult = previewRenderer.render(
                        nativeRenderer, *snapshot, *animatedState, 0, image,
                        EditorModelPreviewTextureRole::BaseColor);
                    assert(animatedStateResult.rendererReady);
                    assert(animatedStateResult.geometryUploaded);
                    nativeAnimatedUpdateExercised = true;

                    nativeRenderer.submit();
                    if (nativeRenderer.last_error().empty()) {
                        nativeOffscreenExecutionExercised = true;
                        render::TextureReadback readback;
                        render::TextureReadbackRequest request;
                        request.texture = previewRenderer.offscreen_color();
                        request.maxBytes = 16u * 1024u * 1024u;
                        if (nativeRenderer.read_texture(request, readback) && readback.valid() &&
                            readback.width > 0 && readback.height > 0) {
                            nativeOffscreenReadbackExercised = true;
                            for (std::size_t pixel = 0; pixel + 3 < readback.data.size(); pixel += 4)
                                nativeOffscreenPixelActivityExercised |= readback.data[pixel] > 16 ||
                                    readback.data[pixel + 1] > 16 || readback.data[pixel + 2] > 16;
                            nativeAnimatedReadbackExercised = nativeOffscreenReadbackExercised &&
                                nativeOffscreenPixelActivityExercised;
                        } else {
                            std::cerr << "native offscreen readback failure: "
                                      << nativeRenderer.last_error() << "\n";
                        }
                        if (!nativeOffscreenPixelActivityExercised) {
                            const auto stats = nativeRenderer.stats();
                            std::cerr << "native offscreen readback was valid but contained no non-zero bytes;"
                                      << " draw-calls=" << stats.drawCalls
                                      << " mesh-calls=" << stats.meshCalls
                                      << " sprite-calls=" << stats.spriteCalls << "\n";
                        }
                        render::TextureDesc readbackTextureDescription;
                        readbackTextureDescription.width = 4;
                        readbackTextureDescription.height = 4;
                        readbackTextureDescription.format = "bgra8";
                        for (unsigned i = 0; i < 4u * 4u * 4u; ++i)
                            readbackTextureDescription.initialData.push_back(static_cast<std::uint8_t>(i * 3u));
                        const auto readbackTexture = nativeRenderer.create_texture(readbackTextureDescription);
                        render::TextureReadback uploadedReadback;
                        render::TextureReadbackRequest uploadedRequest;
                        uploadedRequest.texture = readbackTexture;
                        uploadedRequest.maxBytes = 4u * 4u * 4u;
                        nativeTextureReadbackExercised = nativeRenderer.read_texture(
                            uploadedRequest, uploadedReadback) && uploadedReadback.valid() &&
                            uploadedReadback.data == readbackTextureDescription.initialData;
                        auto regionRequest = uploadedRequest;
                        regionRequest.x = 1; regionRequest.y = 2; regionRequest.width = 2; regionRequest.height = 1;
                        regionRequest.maxBytes = 8;
                        render::TextureReadback region;
                        assert(nativeRenderer.read_texture(regionRequest, region) && region.rowPitch == 8 &&
                            region.width == 2 && region.height == 1);
                        assert(std::equal(region.data.begin(), region.data.end(), readbackTextureDescription.initialData.begin() + 36));
                        regionRequest.maxBytes = 7;
                        assert(!nativeRenderer.read_texture(regionRequest, region));
                        if (!nativeTextureReadbackExercised) {
                            std::cerr << "native uploaded texture readback failure: "
                                      << nativeRenderer.last_error() << "\n";
                        }
                        assert(nativeOffscreenReadbackExercised);
                        assert(nativeOffscreenPixelActivityExercised);
                        assert(nativeTextureReadbackExercised);
                    } else {
                        std::cerr << "native offscreen execution failure: "
                                  << nativeRenderer.last_error() << "\n";
                    }
                }
            }
        }
    }

    if(nativePathExercised) {
        nativeRenderer.begin_graph();nativeRenderer.begin_editor_frame();
        render::EditorViewportSeam independent;
        independent.viewport={0,0,320,160};independent.enabled=true;independent.strictTarget=false;
        assert(nativeRenderer.set_editor_viewport(independent));
        const auto square=previewRenderer.render(nativeRenderer,*snapshot,*sceneState,0,{},
            EditorModelPreviewTextureRole::BaseColor,false);
        assert(square.rendererReady && !square.offscreenCompositeApplied);
        const auto* description=nativeRenderer.resource_description(previewRenderer.offscreen_color());
        const auto* texture=description?std::get_if<render::TextureDesc>(description):nullptr;
        assert(texture && texture->width==512 && texture->height==512);
        independent.viewport={0,0,160,320};assert(nativeRenderer.set_editor_viewport(independent));
        const auto textureBefore=previewRenderer.offscreen_color();
        const auto resized=previewRenderer.render(nativeRenderer,*snapshot,*sceneState,0,{},
            EditorModelPreviewTextureRole::BaseColor,false,true);
        assert(resized.rendererReady && previewRenderer.offscreen_color().id==textureBefore.id);
    }
    if (requireNative) {
        assert(nativePathExercised && nativeOffscreenExecutionExercised && nativeDepthTargetExercised);
        assert(nativeOffscreenReadbackExercised && nativeOffscreenPixelActivityExercised && nativeTextureReadbackExercised);
    }
    std::cout << "Editor model preview renderer backend=" << backendName
              << " native-attempted=" << (nativePathAttempted ? 1 : 0)
              << " native-pass=" << (nativePathExercised ? 1 : 0)
              << " native-material-factors=" << (nativeMaterialFactorsExercised ? 1 : 0)
              << " native-texture-sampled=" << (nativeTextureExercised ? 1 : 0)
              << " native-metallic-roughness-texture="
              << (nativeMetallicRoughnessTextureExercised ? 1 : 0)
              << " native-offscreen-executed=" << (nativeOffscreenExecutionExercised ? 1 : 0)
              << " native-offscreen-readback=" << (nativeOffscreenReadbackExercised ? 1 : 0)
              << " native-offscreen-pixel-activity="
              << (nativeOffscreenPixelActivityExercised ? 1 : 0)
              << " native-animated-update=" << (nativeAnimatedUpdateExercised ? 1 : 0)
              << " native-animated-readback=" << (nativeAnimatedReadbackExercised ? 1 : 0)
              << " native-depth-target=" << (nativeDepthTargetExercised ? 1 : 0)
              << " native-uploaded-texture-readback="
              << (nativeTextureReadbackExercised ? 1 : 0) << "\n";
    previewRenderer.clear(nativeRenderer);
    nativeRenderer.wait_idle();
    if (requireNative) {
        for (const auto& diagnostic : nativeRenderer.stats().validationDiagnostics)
            assert(diagnostic.rfind("error:", 0) != 0);
    }
    }
    nativeWindow.destroy();
    return 0;
}
