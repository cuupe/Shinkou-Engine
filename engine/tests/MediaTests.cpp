#include "shinkou/ui/Media.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace shinkou::ui;

namespace {

MediaPanel video_panel() {
    MediaPanelDescription description;
    description.id = "preview-video";
    description.title = "Preview Video";
    description.kind = MediaKind::Video;
    description.resource = {"asset://video/trailer.mp4", "video/mp4", "Trailer"};
    description.showVolume = true;
    return MediaPanel{description};
}

} // namespace

int main() {
    MediaPanel panel = video_panel();
    assert(panel.valid());
    assert(panel.apply(MediaCommand::set_duration(10.0)) == MediaCommandResult::Applied);
    assert(panel.apply(MediaCommand::set_volume(0.35)) == MediaCommandResult::Applied);
    assert(panel.apply(MediaCommand::set_loop(true)) == MediaCommandResult::Applied);
    assert(panel.apply(MediaCommand::play()) == MediaCommandResult::Applied);
    panel.update(4.0);
    assert(panel.playback().state == MediaPlaybackState::Playing);
    assert(std::abs(panel.playback().currentTime - 4.0) < 0.0001);
    panel.update(8.0);
    assert(panel.playback().state == MediaPlaybackState::Playing);
    assert(std::abs(panel.playback().currentTime - 2.0) < 0.0001);

    assert(panel.apply(MediaCommand::seek(7.0)) == MediaCommandResult::Applied);
    assert(panel.apply(MediaCommand::set_muted(true)) == MediaCommandResult::Applied);
    assert(panel.apply(MediaCommand::pause()) == MediaCommandResult::Applied);
    assert(panel.playback().state == MediaPlaybackState::Paused);

    const std::string compact = panel.to_json();
    MediaPanel restored;
    std::string error;
    assert(restored.from_json(compact, &error));
    assert(restored.description().resource.uri == "asset://video/trailer.mp4");
    assert(restored.description().showVolume);
    assert(restored.playback().muted && restored.playback().loop);
    assert(restored.to_json() == compact);

    const std::string pretty = serialize_json(panel, true);
    MediaPanel prettyRestored;
    assert(deserialize_json(pretty, prettyRestored, &error));
    assert(prettyRestored.to_json() == compact);

    const std::string beforeBad = restored.to_json();
    assert(!restored.from_json("{\"schema\":\"shinkou.ui-media\",\"version\":1,\"description\":{}}", &error));
    assert(restored.to_json() == beforeBad);
    assert(restored.apply(MediaCommand::set_volume(1.1), &error) == MediaCommandResult::Invalid);

    MediaPanel image;
    MediaPanelDescription imageDescription;
    imageDescription.id = "cover";
    imageDescription.title = "Cover";
    imageDescription.kind = MediaKind::Image;
    imageDescription.resource.uri = "asset://textures/cover.png";
    image.set_description(imageDescription);
    assert(image.valid());
    assert(image.apply(MediaCommand::play()) == MediaCommandResult::Ignored);
    assert(image.playback().state == MediaPlaybackState::Stopped);

    MediaPanel stream = video_panel();
    assert(stream.apply(MediaCommand::play()) == MediaCommandResult::Applied);
    stream.update(2.5);
    assert(std::abs(stream.playback().currentTime - 2.5) < 0.0001);

    std::cout << "Media panel model, commands, playback, and JSON passed\n";
    return 0;
}
