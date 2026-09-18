#include "shinkou/editor/EditorModelPreview.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffu));
    output.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    output.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    output.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

void append_f32(std::vector<std::uint8_t>& output, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(output, bits);
}

std::vector<std::uint8_t> triangle_binary() {
    std::vector<std::uint8_t> output;
    output.reserve(102);
    append_f32(output, -1.0f); append_f32(output, -1.0f); append_f32(output, 0.0f);
    append_f32(output, 1.0f); append_f32(output, -1.0f); append_f32(output, 0.0f);
    append_f32(output, 0.0f); append_f32(output, 1.0f); append_f32(output, 0.0f);
    output.push_back(0); output.push_back(0);
    output.push_back(1); output.push_back(0);
    output.push_back(2); output.push_back(0);
    append_f32(output, 0.0f); append_f32(output, 1.0f);
    append_f32(output, 1.0f); append_f32(output, 1.0f);
    append_f32(output, 0.5f); append_f32(output, 0.0f);
    append_f32(output, 0.0f); append_f32(output, 0.0f); append_f32(output, 1.0f);
    append_f32(output, 0.0f); append_f32(output, 0.0f); append_f32(output, 1.0f);
    append_f32(output, 0.0f); append_f32(output, 0.0f); append_f32(output, 1.0f);
    return output;
}

std::vector<std::uint8_t> tiny_png() {
    return {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
        0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0xf8,0xcf,0xc0,
        0x00,0x00,0x03,0x01,0x01,0x00,0xc9,0xfe,0x92,0xef,0x00,0x00,0x00,0x00,0x49,0x45,
        0x4e,0x44,0xae,0x42,0x60,0x82
    };
}

std::string triangle_json() {
    return R"json({
      "asset":{"version":"2.0"},
      "buffers":[{"byteLength":102,"uri":"triangle.bin"}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":36},
        {"buffer":0,"byteOffset":36,"byteLength":6},
        {"buffer":0,"byteOffset":42,"byteLength":24},
        {"buffer":0,"byteOffset":66,"byteLength":36}
      ],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
        {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
        {"bufferView":3,"componentType":5126,"count":3,"type":"VEC3"}
      ],
      "images":[{"name":"Albedo","uri":"textures/albedo.png","mimeType":"image/png"}],
      "samplers":[{}],
      "textures":[{"name":"AlbedoTexture","source":0,"sampler":0}],
      "materials":[{"name":"TriangleMaterial","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.8,1.0],"metallicFactor":0.25,"roughnessFactor":0.75,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}},"alphaMode":"BLEND","doubleSided":true}],
      "animations":[{"name":"Idle","samplers":[{}],"channels":[{"sampler":0}]}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2,"NORMAL":3},"indices":1}]}]
    })json";
}

std::vector<std::uint8_t> triangle_glb() {
    auto json = triangle_json();
    const auto uri = json.find(",\"uri\":\"triangle.bin\"");
    assert(uri != std::string::npos);
    json.erase(uri, std::string(",\"uri\":\"triangle.bin\"").size());
    std::vector<std::uint8_t> jsonChunk(json.begin(), json.end());
    while (jsonChunk.size() % 4u != 0u) jsonChunk.push_back(' ');
    auto binary = triangle_binary();
    while (binary.size() % 4u != 0u) binary.push_back(0);
    std::vector<std::uint8_t> output;
    const auto total = 12u + 8u + static_cast<std::uint32_t>(jsonChunk.size()) +
        8u + static_cast<std::uint32_t>(binary.size());
    append_u32(output, 0x46546c67u);
    append_u32(output, 2u);
    append_u32(output, total);
    append_u32(output, static_cast<std::uint32_t>(jsonChunk.size()));
    append_u32(output, 0x4e4f534au);
    output.insert(output.end(), jsonChunk.begin(), jsonChunk.end());
    append_u32(output, static_cast<std::uint32_t>(binary.size()));
    append_u32(output, 0x004e4942u);
    output.insert(output.end(), binary.begin(), binary.end());
    return output;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("shinkou-editor-gltf-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanup;
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / "assets/textures");
    {
        std::ofstream json(root / "assets/triangle.gltf");
        json << triangle_json();
    }
    {
        const auto bytes = triangle_binary();
        std::ofstream binary(root / "assets/triangle.bin", std::ios::binary);
        binary.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        const auto bytes = tiny_png();
        std::ofstream image(root / "assets/textures/albedo.png", std::ios::binary);
        image.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        const auto bytes = triangle_glb();
        std::ofstream glb(root / "assets/triangle.glb", std::ios::binary);
        glb.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ofstream(root / "assets/broken.glb", std::ios::binary) << "bad";

    shinkou::editor::FileSystemService files(root);
    const auto gltf = shinkou::editor::load_editor_model_preview(files, "assets/triangle.gltf", 4, 10);
    assert(gltf.snapshot && gltf.snapshot->valid());
    assert(gltf.snapshot->sourceFormat == "gltf" && gltf.snapshot->vertexCount == 3 &&
           gltf.snapshot->triangleCount == 1 && gltf.snapshot->meshCount == 1 &&
           gltf.snapshot->primitiveCount == 1 && gltf.snapshot->materialCount == 1 &&
           gltf.snapshot->textureCount == 1 && gltf.snapshot->imageCount == 1 &&
           gltf.snapshot->animationCount == 1 && gltf.snapshot->animations &&
           gltf.snapshot->animations->front().name == "Idle" &&
           gltf.snapshot->animations->front().channelCount == 1 &&
           gltf.snapshot->animations->front().samplerCount == 1 &&
           gltf.snapshot->materials && gltf.snapshot->materials->front().name == "TriangleMaterial" &&
           gltf.snapshot->materials->front().baseColorTexture == 0 &&
           gltf.snapshot->materials->front().metallicRoughnessTexture == 0 &&
           gltf.snapshot->materials->front().alphaMode == "BLEND" &&
           gltf.snapshot->materials->front().doubleSided &&
           gltf.snapshot->textures && gltf.snapshot->textures->front().source == 0 &&
           gltf.snapshot->textureCoordinates && gltf.snapshot->textureCoordinates->size() == 3 &&
           (*gltf.snapshot->textureCoordinates)[1].x == 1.0f &&
           (*gltf.snapshot->textureCoordinates)[2].y == 0.0f &&
           gltf.snapshot->normals && gltf.snapshot->normals->size() == 3 &&
           (*gltf.snapshot->normals)[0].z == 1.0f &&
           gltf.snapshot->images && gltf.snapshot->images->front().mimeType == "image/png" &&
           gltf.snapshot->imageArtifacts && gltf.snapshot->imageArtifacts->size() == 1 &&
           gltf.snapshot->imageArtifacts->front().valid() &&
           gltf.snapshot->imageArtifacts->front().encodedBytes->size() == tiny_png().size());

    const auto glb = shinkou::editor::load_editor_model_preview(files, "assets/triangle.glb", 5, 11);
    assert(glb.snapshot && glb.snapshot->valid());
    assert(glb.snapshot->sourceFormat == "glb" && glb.snapshot->vertexCount == 3 && glb.snapshot->triangleCount == 1);
    const auto glbSource = triangle_glb();
    const auto glbFromAssetSystemBytes = shinkou::editor::load_editor_gltf_preview_bytes(
        files, "assets/triangle.glb", glbSource, 51, 111);
    assert(glbFromAssetSystemBytes.snapshot && glbFromAssetSystemBytes.snapshot->valid() &&
           glbFromAssetSystemBytes.snapshot->imageArtifacts &&
           glbFromAssetSystemBytes.snapshot->imageArtifacts->size() == 1);
    auto dataUriJson = triangle_json();
    const auto externalImageUri = dataUriJson.find("textures/albedo.png");
    assert(externalImageUri != std::string::npos);
    dataUriJson.replace(externalImageUri, std::string("textures/albedo.png").size(),
                        "data:image/png;base64,AA==");
    std::ofstream(root / "assets/data-image.gltf") << dataUriJson;
    const auto dataUri = shinkou::editor::load_editor_model_preview(
        files, "assets/data-image.gltf", 52, 112);
    assert(dataUri.snapshot && dataUri.snapshot->imageArtifacts &&
           dataUri.snapshot->imageArtifacts->size() == 1 &&
           dataUri.snapshot->imageArtifacts->front().encodedBytes &&
           dataUri.snapshot->imageArtifacts->front().encodedBytes->size() == 1 &&
           dataUri.snapshot->imageArtifacts->front().encodedBytes->front() == 0);
    auto bufferViewImageJson = triangle_json();
    const auto imageObject = bufferViewImageJson.find(
        R"("images":[{"name":"Albedo","uri":"textures/albedo.png","mimeType":"image/png"}])");
    assert(imageObject != std::string::npos);
    bufferViewImageJson.replace(imageObject,
        std::string(R"("images":[{"name":"Albedo","uri":"textures/albedo.png","mimeType":"image/png"}])").size(),
        R"("images":[{"name":"Albedo","bufferView":1,"mimeType":"image/png"}])");
    std::ofstream(root / "assets/buffer-view-image.gltf") << bufferViewImageJson;
    const auto bufferViewImage = shinkou::editor::load_editor_model_preview(
        files, "assets/buffer-view-image.gltf", 53, 113);
    assert(bufferViewImage.snapshot && bufferViewImage.snapshot->imageArtifacts &&
           bufferViewImage.snapshot->imageArtifacts->size() == 1 &&
           bufferViewImage.snapshot->imageArtifacts->front().encodedBytes &&
           bufferViewImage.snapshot->imageArtifacts->front().encodedBytes->size() == 6);
    auto invalidMetallicRoughnessJson = triangle_json();
    const auto metallicRoughnessSlot = invalidMetallicRoughnessJson.find(
        "\"metallicRoughnessTexture\":{\"index\":0}");
    assert(metallicRoughnessSlot != std::string::npos);
    invalidMetallicRoughnessJson.replace(
        metallicRoughnessSlot, std::string("\"metallicRoughnessTexture\":{\"index\":0}").size(),
        "\"metallicRoughnessTexture\":{\"index\":9}");
    std::ofstream(root / "assets/invalid-metallic-roughness.gltf") << invalidMetallicRoughnessJson;
    const auto invalidMetallicRoughness = shinkou::editor::load_editor_model_preview(
        files, "assets/invalid-metallic-roughness.gltf", 54, 114);
    assert(!invalidMetallicRoughness.snapshot &&
           invalidMetallicRoughness.error.find("metallicRoughnessTexture index is invalid") !=
               std::string::npos);

    std::atomic_bool cancelled{true};
    const auto cancelledResult = shinkou::editor::load_editor_model_preview(
        files, "assets/triangle.glb", 6, 12, &cancelled);
    assert(!cancelledResult.snapshot && cancelledResult.error.find("cancelled") != std::string::npos);

    const auto broken = shinkou::editor::load_editor_model_preview(files, "assets/broken.glb", 7, 13);
    assert(!broken.snapshot && broken.error.find("GLB") != std::string::npos);
    auto unsafeJson = triangle_json();
    const auto imageUri = unsafeJson.find("textures/albedo.png");
    assert(imageUri != std::string::npos);
    unsafeJson.replace(imageUri, std::string("textures/albedo.png").size(), "../outside.png");
    std::ofstream(root / "assets/unsafe-image.gltf") << unsafeJson;
    const auto unsafe = shinkou::editor::load_editor_model_preview(files, "assets/unsafe-image.gltf", 8, 14);
    assert(!unsafe.snapshot && unsafe.error.find("project-relative") != std::string::npos);
    const auto outside = shinkou::editor::load_editor_model_preview(files, "../outside.glb", 9, 15);
    assert(!outside.snapshot && !outside.error.empty());
    std::filesystem::remove_all(root, cleanup);
    std::cout << "Editor glTF/GLB preview provider passed\n";
    return 0;
}
