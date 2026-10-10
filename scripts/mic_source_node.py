#!/usr/bin/env python3
# Copyright 2026 TOPSBOT
# SPDX-License-Identifier: Apache-2.0

"""Realtime microphone capture -> /audio_raw.

Reads raw s16le PCM from an ALSA capture device via `arecord` and publishes it
as std_msgs/msg/Int8MultiArray, byte-for-byte the same format that
`audio_source_node.py` produces when replaying a file. whisper_asr_node
downstream is agnostic to the source, so this is a drop-in real-mic front end.

The capture device is opened in stereo (`-c 2`) and down-mixed to mono here.
Reason: the ES8388 mic on the target board is wired as a differential pair
(L = +mic, R = -mic). Asking arecord for `-c 1` makes plughw average (L+R)/2,
which cancels the speech and leaves only noise -- the observed "Detect language
error" + garbled output. `--mix difference` (default) computes (L-R)/2 and
recovers the speech while rejecting common-mode noise; `--mix left`/`right`
take one channel verbatim, `--mix average` reproduces the old (L+R)/2 behavior.
"""

import argparse
import array
import subprocess
import sys

import rclpy
from rclpy.node import Node
from std_msgs.msg import Int8MultiArray


class MicSourceNode(Node):
    def __init__(self, device, topic_cli, sample_rate_cli, chunk_seconds, mix):
        super().__init__("whisper_mic_source")
        self.declare_parameter("topic", topic_cli or "/audio_raw")
        self.declare_parameter("sample_rate", sample_rate_cli or 16000)
        self.declare_parameter("mix", mix or "difference")

        topic = self.get_parameter("topic").get_parameter_value().string_value
        sample_rate = self.get_parameter("sample_rate").get_parameter_value().integer_value
        self.mix = self.get_parameter("mix").get_parameter_value().string_value

        self.publisher = self.create_publisher(Int8MultiArray, topic, 10)

        # arecord captures the codec's native stereo; we mix to mono below.
        self.chunk_samples = int(sample_rate * chunk_seconds)
        self.read_bytes = self.chunk_samples * 2 * 2  # samples * 2ch * 2 bytes/sample

        self.cmd = [
            "arecord", "-q",
            "-D", device,
            "-f", "S16_LE",
            "-r", str(sample_rate),
            "-c", "2",
            "-t", "raw",
        ]
        self.proc = subprocess.Popen(
            self.cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.get_logger().info(
            f"capturing {device} at {sample_rate}Hz stereo, mixing "
            f"'{self.mix}' to mono, publishing {self.chunk_samples}-sample "
            f"chunks to {topic}")

    def _read_exact(self, size):
        # pipe read() may return short; accumulate so L/R sample pairs stay aligned.
        buf = bytearray()
        while len(buf) < size:
            data = self.proc.stdout.read(size - len(buf))
            if not data:
                return bytes(buf)
            buf += data
        return bytes(buf)

    def _mix(self, samples):
        n = len(samples) // 2
        if self.mix == "left":
            return array.array('h', samples[0::2])
        if self.mix == "right":
            return array.array('h', samples[1::2])
        if self.mix == "average":
            return array.array(
                'h', ((samples[2 * i] + samples[2 * i + 1]) >> 1 for i in range(n)))
        # difference (default): differential mic, (L - R) / 2
        return array.array(
            'h', ((samples[2 * i] - samples[2 * i + 1]) >> 1 for i in range(n)))

    def stream(self):
        while rclpy.ok():
            raw = self._read_exact(self.read_bytes)
            if len(raw) < self.read_bytes:
                self.get_logger().error(
                    f"arecord exited early (ret={self.proc.poll()}): {' '.join(self.cmd)}")
                return
            samples = array.array('h')
            samples.frombytes(raw)
            mono = self._mix(samples)
            payload = mono.tobytes()
            message = Int8MultiArray()
            # payload is raw s16le bytes; reinterpret as signed to fit int8[].
            message.data = [b if b < 128 else b - 256 for b in payload]
            self.publisher.publish(message)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="plughw:0,0",
                        help="ALSA capture device (default: plughw:0,0)")
    parser.add_argument("--topic", default=None)
    parser.add_argument("--sample-rate", type=int, default=None)
    parser.add_argument("--mix", default=None,
                        choices=["difference", "left", "right", "average"],
                        help="stereo→mono mix (default: difference)")
    parser.add_argument("--chunk-seconds", type=float, default=0.1)

    # rclpy must consume `--ros-args` before argparse sees argv.
    rclpy.init(args=sys.argv)
    args, _ = parser.parse_known_args()

    node = MicSourceNode(
        args.device, args.topic, args.sample_rate, args.chunk_seconds, args.mix)
    try:
        node.stream()
    finally:
        node.stop()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    sys.exit(main())