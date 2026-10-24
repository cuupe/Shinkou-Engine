#include "shinkou/editor/EditorAssetSettings.h"
#include "shinkou/reflection/Serialization.h"
#include <cmath>

namespace shinkou::reflection {
SHINKOU_REFLECT_TYPE(::shinkou::editor::EditorAssetSettings,"editor.AssetSettings");
}
namespace shinkou::editor {
namespace {
void register_settings(reflection::TypeRegistry& registry) {
    using S=EditorAssetSettings;
    registry.register_type(reflection::TypeBuilder<S>("editor.AssetSettings")
        .field("version",&S::version).field("wordWrap",&S::wordWrap).field("lineNumbers",&S::lineNumbers)
        .field("documentStyles",&S::documentStyles).field("textScale",&S::textScale)
        .field("imageCheckerboard",&S::imageCheckerboard).field("imageZoom",&S::imageZoom)
        .field("loop",&S::loop).field("streaming",&S::streaming).field("spatialized",&S::spatialized)
        .field("bus",&S::bus).field("volume",&S::volume).field("pitch",&S::pitch).field("pan",&S::pan)
        .field("fadeIn",&S::fadeIn).field("minDistance",&S::minDistance).field("maxDistance",&S::maxDistance)
        .field("rolloff",&S::rolloff).field("positionX",&S::positionX).field("positionY",&S::positionY).field("positionZ",&S::positionZ)
        .field("lowPass",&S::lowPass).field("reverb",&S::reverb).field("cutoffHz",&S::cutoffHz).field("reverbWet",&S::reverbWet)
        .field("videoRate",&S::videoRate).field("modelAutoRotate",&S::modelAutoRotate).field("modelWireframe",&S::modelWireframe).take());
}
bool range(float v,float lo,float hi) {return std::isfinite(v) && v>=lo && v<=hi;}
}
bool EditorAssetSettings::valid() const noexcept {
    return version==1 && bus<static_cast<unsigned>(audio::AudioBus::Count) &&
        range(textScale,0.75f,2) && range(imageZoom,0.25f,4) && range(volume,0,1) && range(pitch,0.01f,8) &&
        range(pan,-1,1) && range(fadeIn,0,10) && range(minDistance,0.01f,100000) && range(maxDistance,minDistance,100000) &&
        range(rolloff,0,10) && range(positionX,-100000,100000) && range(positionY,-100000,100000) && range(positionZ,-100000,100000) &&
        range(cutoffHz,20,20000) && range(reverbWet,0,1) && range(videoRate,0.1f,4);
}
audio::AudioPlayParams EditorAssetSettings::audio_params(audio::AudioTrackId track) const noexcept {
    audio::AudioPlayParams p;
    p.bus=static_cast<audio::AudioBus>(bus);p.track=track;p.loop=loop;p.streaming=streaming;
    p.spatialized=spatialized;p.volume=volume;p.pitch=pitch;p.pan=pan;p.fadeInSeconds=fadeIn;
    p.minDistance=minDistance;p.maxDistance=maxDistance;p.rolloff=rolloff;p.position={positionX,positionY,positionZ};return p;
}
std::vector<audio::AudioEffectDesc> EditorAssetSettings::audio_effects() const {
    std::vector<audio::AudioEffectDesc> effects;
    if(lowPass) {audio::AudioEffectDesc d;d.type=audio::AudioEffectType::LowPass;d.cutoffHz=cutoffHz;effects.push_back(d);}
    if(reverb) {audio::AudioEffectDesc d;d.type=audio::AudioEffectType::Reverb;d.wet=reverbWet;effects.push_back(d);}
    return effects;
}
std::filesystem::path EditorAssetSettingsStore::path_for(std::string_view asset) {
    const auto p=std::filesystem::u8path(asset).lexically_normal();
    if(p.empty() || p=="." || p.is_absolute() || p.has_root_name()) return {};
    for(const auto& component:p) if(component=="..") return {};
    return std::filesystem::path("Saved/Editor/AssetSettings")/p/"settings.json";
}
bool EditorAssetSettingsStore::load(const FileSystemService& files,std::string_view asset,EditorAssetSettings& out,std::string& error) {
    error.clear();const auto path=path_for(asset);
    if(path.empty()) {error="Invalid asset settings path";return false;}
    if(!files.exists(path)) {out={};return true;}
    std::string json;bool truncated=false;
    if(!files.read_text_limited(path,64u*1024u,json,&truncated,&error)) return false;
    if(truncated) {error="Asset settings exceed 64 KiB";return false;}
    reflection::TypeRegistry registry;register_settings(registry);EditorAssetSettings next;
    const auto parsed=reflection::deserialize_json(registry,reflection::type_id<EditorAssetSettings>(),json,&next);
    if(!parsed) {error=parsed.message;return false;}
    if(!next.valid()) {error="Asset settings contain unsupported or out-of-range values";return false;}
    out=next;return true;
}
bool EditorAssetSettingsStore::save(const FileSystemService& files,std::string_view asset,const EditorAssetSettings& settings,std::string& error) {
    error.clear();const auto path=path_for(asset);
    if(path.empty() || !settings.valid()) {error="Invalid asset settings";return false;}
    reflection::TypeRegistry registry;register_settings(registry);std::string json;
    const auto serialized=reflection::serialize_json(registry,reflection::type_id<EditorAssetSettings>(),&settings,json,{true});
    if(!serialized) {error=serialized.message;return false;}
    return files.ensure_directory(path.parent_path(),&error) && files.write_text_atomic(path,json,&error);
}
bool EditorAssetSettingsStore::remap(const FileSystemService& files,std::string_view from,std::string_view to,std::string& error) {
    const auto a=path_for(from).parent_path(),b=path_for(to).parent_path();
    if(a.empty() || b.empty()) {error="Invalid settings migration path";return false;}
    if(a==b || !files.exists(a)) return true;
    return files.ensure_directory(b.parent_path(),&error) && files.rename(a,b,&error);
}
}
