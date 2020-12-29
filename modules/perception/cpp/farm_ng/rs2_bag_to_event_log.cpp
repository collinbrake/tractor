/* Project docs and todo list:
 *   https://docs.google.com/document/d/1cvZWOBCO7R57tfYvI6r97d-aeuYCi-Os10OqYn9Duqo/edit?usp=sharing
 */

#include <iostream>
#include <optional>

#include <google/protobuf/util/time_util.h>
#include <librealsense2/rs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include "farm_ng/core/blobstore.h"
#include "farm_ng/core/init.h"
#include "farm_ng/core/ipc.h"

#include "farm_ng/perception/camera_model.h"
#include "farm_ng/perception/camera_pipeline.pb.h"
#include "farm_ng/perception/image.pb.h"
#include "farm_ng/perception/intel_rs2_utils.h"
#include "farm_ng/perception/rs2_bag_to_event_log.pb.h"
#include "farm_ng/perception/video_streamer.h"

DEFINE_bool(interactive, false, "receive program args via eventbus");
DEFINE_string(name, "default",
              "a dataset name, used in the output archive name");
DEFINE_string(rs2_bag_path, "", "A RealSense bag file path.");
DEFINE_string(camera_frame_name, "camera01",
              "Frame name to use for the camera model.");

typedef farm_ng::core::Event EventPb;
using farm_ng::core::ArchiveProtobufAsJsonResource;
using farm_ng::core::EventBus;
using farm_ng::core::LoggingStatus;
using farm_ng::core::MakeEvent;
using farm_ng::core::MakeTimestampNow;
using farm_ng::core::Subscription;

namespace farm_ng {
namespace perception {

class Rs2BagToEventLogProgram {
 public:
  Rs2BagToEventLogProgram(EventBus& bus,
                          Rs2BagToEventLogConfiguration configuration,
                          bool interactive)
      : bus_(bus), timer_(bus_.get_io_service()) {
    if (interactive) {  // The program doesn't use interactive mode at this
                        // point
      status_.mutable_input_required_configuration()->CopyFrom(configuration);
    } else {
      set_configuration(configuration);
    }
    bus_.GetEventSignal()->connect(std::bind(&Rs2BagToEventLogProgram::on_event,
                                             this, std::placeholders::_1));
    bus_.AddSubscriptions({bus_.GetName(), "logger/command", "logger/status"});
  }

  int run() {
    // Get necessary config from event bus
    while (status_.has_input_required_configuration()) {
      bus_.get_io_service().run_one();
    }

    WaitForServices(bus_, {"ipc_logger"});

    Rs2BagToEventLogResult result;

    result.mutable_stamp_begin()->CopyFrom(MakeTimestampNow());

    // Start a realsense pipeline from a recorded file to get the framesets
    rs2::pipeline pipe;
    rs2::config cfg;

    // Set the optional repeat_playback arg to false
    cfg.enable_device_from_file((farm_ng::core::GetBlobstoreRoot() /
                                 configuration_.rs2_bag_resource().path())
                                    .string(),
                                false);

    rs2::pipeline_profile selection = pipe.start(cfg);
    auto color_stream =
        selection.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();

    CameraModel camera_model;
    camera_model.set_frame_name(configuration_.camera_frame_name());
    SetCameraModelFromRs(&camera_model, color_stream.get_intrinsics());

    // This variable will be populated by the video streamer. Declare
    // outside of the while loop to access for use by result message.
    Image image_pb;

    std::unique_ptr<VideoStreamer> streamer = std::make_unique<VideoStreamer>(
        bus_, camera_model,
        // later we will replace MODE_MP4_FILE with MODE_JPG_SEQUENCE
        // but that's not implemented yet
        VideoStreamer::MODE_MP4_FILE);

    LoggingStatus log = StartLogging(bus_, configuration_.name());

    while (true) {
      // Fetch the next frameset (block until it comes)
      rs2::frameset frames = pipe.wait_for_frames().as<rs2::frameset>();

      // Get the depth and color frames
      rs2::video_frame color_frame = frames.get_color_frame();
      // rs2::depth_frame depth_frame = frames.get_depth_frame();

      // Query frame size (width and height)
      const int wc = color_frame.get_width();
      const int hc = color_frame.get_height();
      // const int wd = depth_frame.get_width();
      // const int hd = depth_frame.get_height();

      CHECK_EQ(camera_model.image_width(), wc);
      CHECK_EQ(camera_model.image_height(), hc);

      // Image matrices from rs2 frames
      cv::Mat color = RS2FrameToMat(color_frame);
      // cv::Mat depth = RS2FrameToMat(depth_frame),

      if (color.empty()) {
        break;
      }

      // Display the current video frame, using case insensitive q as quit
      // signal.
      cv::imshow("Color", color);
      char c = static_cast<char>(cv::waitKey(1));
      if (tolower(c) == 'q') {
        std::cout << "Quit signal recieved, stopping video.\n\n";
        break;
      }

      // Add timestamped frame with the video streamer
      auto frame_stamp =
          google::protobuf::util::TimeUtil::MillisecondsToTimestamp(
              color_frame.get_timestamp());
      image_pb = streamer->AddFrame(color, frame_stamp);

      // std::cout << "Wrote frame number: " << image_pb.frame_number().value()
      //          << std::endl;

      // Send out the image Protobuf on the event bus
      auto stamp = core::MakeTimestampNow();
      bus_.Send(
          MakeEvent(camera_model.frame_name() + "/image", image_pb, stamp));

      // zero index base for the frame_number, set after send.
      image_pb.mutable_frame_number()->set_value(
          image_pb.frame_number().value() + 1);
    }

    result.mutable_configuration()->CopyFrom(configuration_);
    result.mutable_dataset()->CopyFrom(image_pb.resource());
    result.mutable_stamp_end()->CopyFrom(MakeTimestampNow());

    ArchiveProtobufAsJsonResource(configuration_.name(), result);

    LOG(INFO) << "Complete:\n" << status_.DebugString();

    status_.set_num_frames(image_pb.frame_number().value());
    send_status();
    return 0;
  }

  void send_status() {
    bus_.Send(MakeEvent(bus_.GetName() + "/status", status_));
  }

  bool on_configuration(const EventPb& event) {
    Rs2BagToEventLogConfiguration configuration;
    if (!event.data().UnpackTo(&configuration)) {
      return false;
    }
    LOG(INFO) << configuration.ShortDebugString();
    set_configuration(configuration);
    return true;
  }

  void set_configuration(Rs2BagToEventLogConfiguration configuration) {
    configuration_ = configuration;
    status_.clear_input_required_configuration();
    send_status();
  }

  void on_event(const EventPb& event) {
    if (on_configuration(event)) {
      return;
    }
  }

 private:
  EventBus& bus_;
  boost::asio::deadline_timer timer_;
  Rs2BagToEventLogConfiguration configuration_;
  Rs2BagToEventLogStatus status_;
};

}  // namespace perception
}  // namespace farm_ng

int Main(farm_ng::core::EventBus& bus) {
  farm_ng::perception::Rs2BagToEventLogConfiguration config;
  config.set_name(FLAGS_name);
  config.set_camera_frame_name(FLAGS_camera_frame_name);
  config.mutable_rs2_bag_resource()->set_path(FLAGS_rs2_bag_path);
  config.mutable_rs2_bag_resource()->set_content_type("rosBag/rs2");

  farm_ng::perception::Rs2BagToEventLogProgram program(bus, config,
                                                       FLAGS_interactive);
  return program.run();
}

void Cleanup(farm_ng::core::EventBus& bus) {
  farm_ng::core::RequestStopLogging(bus);
  LOG(INFO) << "Requested Stop logging";
}

int main(int argc, char* argv[]) {
  return farm_ng::core::Main(argc, argv, &Main, &Cleanup);
}