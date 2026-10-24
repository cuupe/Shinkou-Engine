#include "shinkou/editor/EditorTextPreview.h"
#include "shinkou/editor/EditorAssetSettings.h"
#include "shinkou/editor/EditorModelPreview.h"
#include "shinkou/editor/AssetPreview.h"
#include <miniz.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>

using namespace shinkou::editor;
namespace {
std::string package(std::string_view xml) {
    mz_zip_archive zip{};assert(mz_zip_writer_init_heap(&zip,0,0));
    assert(mz_zip_writer_add_mem(&zip,"word/document.xml",xml.data(),xml.size(),MZ_BEST_COMPRESSION));
    void* data=nullptr;std::size_t size=0;
    assert(mz_zip_writer_finalize_heap_archive(&zip,&data,&size));
    std::string bytes(static_cast<const char*>(data),size);mz_free(data);assert(mz_zip_writer_end(&zip));return bytes;
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("shinkou-asset-inspection-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    FileSystemService files(root);std::string error;
    assert(files.write_text_atomic("assets/code.py","print('preview only')\nline 2\n"));
    auto text=load_editor_text_preview(files,"assets/code.py",3,5);
    assert(text.snapshot && text.snapshot->lines.size()==2 && text.generation==3 && text.sourceStamp==5);
    assert(text.snapshot->lines[1]=="line 2");
    std::string many;for(int i=0;i<80;++i) many+="source line "+std::to_string(i)+"\n";
    assert(files.write_text_atomic("assets/many.lua",many));
    assert(load_editor_text_preview(files,"assets/many.lua",0,0).snapshot->lines.size()==80);
    const std::string utf16{"\xff\xfe\x2d\x4e\x87\x65\x3d\xd8\x00\xde",10};
    assert(files.write_text_atomic(std::filesystem::u8path(u8"assets/中文.txt"),utf16));
    text=load_editor_text_preview(files,u8"assets/中文.txt",0,0);
    assert(text.snapshot && text.snapshot->encoding=="UTF-16 LE" && text.snapshot->lines[0]==u8"中文😀");
    assert(files.write_text_atomic("assets/invalid.txt",std::string("\xed\xa0\x80",3)));
    assert(!load_editor_text_preview(files,"assets/invalid.txt",0,0).snapshot);
    assert(!load_editor_text_preview(files,"../outside.txt",0,0).snapshot);
    const std::string xml=u8R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>文档标题</w:t></w:r></w:p><w:p><w:r><w:t>A &amp; B</w:t><w:tab/><w:t>内容</w:t></w:r></w:p><w:tbl><w:tr><w:tc><w:p><w:r><w:t>第一列</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Second</w:t></w:r></w:p></w:tc></w:tr></w:tbl></w:body></w:document>)";
    assert(files.write_text_atomic("assets/document.docx",package(xml)));
    text=load_editor_text_preview(files,"assets/document.docx",7,11);
    assert(text.snapshot && text.snapshot->styles[0]==1 && text.snapshot->styles[2]==10);
    assert(text.snapshot->lines[0]==u8"文档标题" && text.snapshot->lines[1]==u8"A & B\t内容" && text.snapshot->lines[2]==u8"第一列 | Second");
    assert(files.write_text_atomic("assets/invalid.docx",package("<bad>")));
    assert(!load_editor_text_preview(files,"assets/invalid.docx",0,0).snapshot);
    assert(files.write_text_atomic("assets/oversized.docx",package(std::string(8u*1024u*1024u+1,'x'))));
    assert(!load_editor_text_preview(files,"assets/oversized.docx",0,0).snapshot);
    assert(files.write_text_atomic("assets/package.zip",package(xml)));
    text=load_editor_text_preview(files,"assets/package.zip",0,0);
    assert(text.snapshot && text.snapshot->lines[0].find("word/document.xml")!=std::string::npos);
    EditorAssetSettings settings;settings.pitch=1.5f;settings.pan=-0.4f;settings.bus=2;settings.loop=true;
    settings.spatialized=true;settings.minDistance=2;settings.maxDistance=20;settings.lowPass=true;settings.reverb=true;settings.videoRate=0.5f;
    assert(EditorAssetSettingsStore::save(files,u8"assets/中文.wav",settings,error));
    EditorAssetSettings loaded;
    assert(EditorAssetSettingsStore::load(files,u8"assets/中文.wav",loaded,error));
    assert(loaded.pitch==1.5f && loaded.pan==-0.4f && loaded.loop && loaded.spatialized);
    const auto params=loaded.audio_params(42);assert(params.track==42 && params.bus==shinkou::audio::AudioBus::Sfx && params.pitch==1.5f && params.pan==-0.4f);
    assert(loaded.audio_effects().size()==2);
    assert(EditorAssetSettingsStore::remap(files,"assets","renamed",error));
    assert(EditorAssetSettingsStore::load(files,u8"renamed/中文.wav",loaded,error) && loaded.pitch==1.5f);
    assert(EditorAssetSettingsStore::path_for("../../escape").empty());
    settings.pitch=NAN;assert(!EditorAssetSettingsStore::save(files,"bad.wav",settings,error));
    const auto saved=EditorAssetSettingsStore::path_for(u8"renamed/中文.wav");
    assert(files.write_text_atomic(saved,"{\"version\":99}"));
    assert(!EditorAssetSettingsStore::load(files,u8"renamed/中文.wav",loaded,error) && loaded.pitch==1.5f);
    assert(AssetPreviewCatalog::classify("a.DOCX",false)==AssetPreviewKind::Document);
    const char* fbx=R"(; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: {
 Version: 1000
 Properties70: { P: "UnitScaleFactor", "double", "Number", "",100 }
}
Objects: {
 Geometry: 1, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: -1,0,0,1,0,0,0,2,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 2, "Model::Triangle", "Mesh" {
  Version: 232
  Properties70: { P: "Lcl Translation", "Lcl Translation", "", "A",10,0,0 }
 }
}
Connections: {
 C: "OO",1,2
 C: "OO",2,0
}
)";
    assert(files.write_text_atomic("assets/triangle.fbx",fbx));
    const auto model=load_editor_model_preview(files,"assets/triangle.fbx",4,9);
    if(!model.snapshot) std::cerr<<model.error<<'\n';
    assert(model.snapshot && model.snapshot->valid() && model.snapshot->sourceFormat=="fbx");
    assert(model.snapshot->triangleCount==1 && model.snapshot->normals->size()==3);
    assert(model.snapshot->minX>1); // Node translation is applied to flattened geometry.
    std::filesystem::copy_file(std::filesystem::path(SHINKOU_EDITOR_TEST_FIXTURE_DIR)/"ufbx-cube-7400-binary.fbx",root/"assets/cube.fbx");
    const auto binary=load_editor_model_preview(files,"assets/cube.fbx",5,10);
    if(!binary.snapshot) std::cerr<<binary.error<<'\n';
    assert(binary.snapshot && binary.snapshot->valid() && binary.snapshot->triangleCount==12);
    assert(binary.snapshot->normals->size()==binary.snapshot->vertexCount && binary.snapshot->textureCoordinates->size()==binary.snapshot->vertexCount);
    std::atomic_bool cancel{true};assert(!load_editor_model_preview(files,"assets/triangle.fbx",0,0,&cancel).snapshot);
    assert(files.write_text_atomic("assets/broken.fbx","not fbx"));
    assert(!load_editor_model_preview(files,"assets/broken.fbx",0,0).snapshot);
    assert(files.remove("assets"));assert(files.remove("Saved"));
    std::filesystem::remove(root);
    std::cout<<"Asset inspection providers and settings passed\n";
}
