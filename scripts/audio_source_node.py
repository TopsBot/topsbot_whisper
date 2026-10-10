#!/usr/bin/env python3
# Copyright 2026 TOPSBOT
# SPDX-License-Identifier: Apache-2.0

"""Joint-test audio source: read a WAV/PCM file and publish it as
std_msgs/msg/Int8MultiArray (s16le bytes) so the whisper_asr_node can
transcribe it end to end without microphone hardware.

The file is streamed in chunks so the listener's segmentacumulation and idle
flush paths are exercised the same way a live mic feed would be.
"""

import argparse
import sys
import time
import wave

import rclpy
from rclpy.node import Node
from std_msgs.msg import Int8MultiArray


class AudioSourceNode(Node):
    def __init__(self, path, topic_cli, sample_rate_cli, channels_cli, chunk_seconds,
                 start_delay, loop):
        super().__init__("whisper_audio_source")
        # Declare parameters so launch `parameters=[{...}]` overrides take effect.
        self.declare_parameter("topic", topic_cli or "/audio_raw")
        self.declare_parameter("sample_rate", sample_rate_cli or 16000)
        self.declare_parameter("channels", channels_cli or 1)

        topic = self.get_parameter("topic").get_parameter_value().string_value
        sample_rate = self.get_parameter("sample_rate").get_parameter_value().integer_value
        channels = self.get_parameter("channels").get_parameter_value().integer_value

        self.publisher = self.create_publisher(Int8MultiArray, topic, 10)
        self.loop = loop
        # Start delay lets the ASR node finish loading the model (which takes a
        # few seconds) before we start streaming, so no early audio is dropped.
        self.start_delay = start_delay
        # Sleep exactly one chunk_seconds per chunk so the stream plays back in
        # real time (a fixed sleep independent of chunk size would fast-forward).
        self.chunk_seconds = chunk_seconds
        self.chunk_bytes = int(sample_rate * channels * 2 * chunk_seconds)
        self.frames = self._load(path, sample_rate, channels)
        if not self.frames:
            raise RuntimeError(f"no audio frames loaded from {path}")
        self.get_logger().info(
            f"loaded {len(self.frames)} bytes from {path}; "
            f"publishing to {topic} in {self.chunk_bytes}-byte chunks "
            f"(loop={loop})"
        )

    @staticmethod
    def _load(path, sample_rate, channels):
        if path.lower().endswith(".wav"):
            with wave.open(path, "rb") as wav:
                src_rate = wav.getframerate()
                src_channels = wav.getnchannels()
                width = wav.getsampwidth()
                if src_rate != sample_rate or src_channels != channels or width != 2:
                    raise RuntimeError(
                        f"expected {sample_rate}Hz/{channels}ch/16-bit wav, got "
                        f"{src_rate}Hz/{src_channels}ch/{width * 8}-bit"
                    )
                return wav.readframes(wav.getnframes())
        # Raw s16le fallback.
        with open(path, "rb") as handle:
            return handle.read()

    def stream(self):
        if self.loop:
            self.get_logger().info(f"waiting {self.start_delay}s before streaming (loop)")
        else:
            # One-shot: wait start_delay so the ASR node can load its model.
            self.get_logger().info(f"waiting {self.start_delay}s for ASR node to load")
            time.sleep(self.start_delay)
        offset = 0
        while rclpy.ok():
            chunk = self.frames[offset:offset + self.chunk_bytes]
            if not chunk:
                if not self.loop:
                    self.get_logger().info("stream finished; exiting")
                    return
                offset = 0
                continue
            message = Int8MultiArray()
            # chunk is raw s16le bytes; reinterpret as signed to fit int8[].
            message.data = [b if b < 128 else b - 256 for b in chunk]
            self.publisher.publish(message)
            offset += len(chunk)
            time.sleep(self.chunk_seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio_file")
    parser.add_argument("--topic", default=None)
    parser.add_argument("--sample-rate", type=int, default=None)
    parser.add_argument("--channels", type=int, default=None)
    parser.add_argument("--chunk-seconds", type=float, default=0.1)
    parser.add_argument("--start-delay", type=float, default=5.0)
    parser.add_argument("--loop", action="store_true")

    # rclpy must consume `--ros-args` (node remap + launch --params-file) before
    # argparse sees argv; parse_known_args() then drops any leftover ROS args.
    rclpy.init(args=sys.argv)
    args, _ = parser.parse_known_args()
    node = AudioSourceNode(
        args.audio_file, args.topic, args.sample_rate, args.channels,
        args.chunk_seconds, args.start_delay, args.loop)
    try:
        node.stream()
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    sys.exit(main())