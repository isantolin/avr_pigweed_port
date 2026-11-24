#include <Arduino.h>
#include <PigweedAVR.h>

namespace {

class NullWriter final : public pw::stream::NonSeekableWriter {
 private:
  pw::Status DoWrite(pw::ConstByteSpan) override { return pw::OkStatus(); }
};

NullWriter writer;
pw::hdlc::RpcChannelOutput output(writer, 1, "mega-metric-export-probe");
pw::rpc::Channel channels[] = {
    pw::rpc::Channel::Create<1>(&output)};
pw::rpc::Server server(pw::span<pw::rpc::Channel>(channels, 1));

PW_METRIC_GROUP_STATIC(bridge_group, "bridge");
PW_METRIC_STATIC(bridge_group, rx_frames, "rx_frames", 0u);
PW_METRIC_TYPED_STATIC(bridge_group, link_up, "link_up", bool, false);
pw::metric::MetricList root_metrics;
pw::metric::GroupList root_groups;
pw::metric::MetricService metric_service(root_metrics, root_groups);

}  // namespace

void setup() {
  root_groups.push_front(bridge_group);
  link_up.Set(true);
  server.RegisterService(metric_service);
}

void loop() {
  // Link-time probe only: exercises atomic shims and pwpb MetricService.
  rx_frames.Increment();
  if (!link_up.value()) {
    rx_frames.Decrement();
  }
}
