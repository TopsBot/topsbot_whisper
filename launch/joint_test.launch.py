# Copyright 2026 TOPSBOT
# SPDX-License-Identifier: Apache-2.0

import os
import sys

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

_launch_dir = os.path.dirname(os.path.abspath(__file__))
if _launch_dir not in sys.path:
    sys.path.insert(0, _launch_dir)
from model_utils import (  # noqa: E402
    ensure_whisper_models,
    load_params,
    resolve_params_file,
)


def _launch_setup(context):
    package_share = get_package_share_directory("topsbot_whisper_asr")
    params_path = resolve_params_file(
        package_share, LaunchConfiguration("params_file").perform(context))
    params = load_params(params_path)
    requested_model = LaunchConfiguration("model_path").perform(context)
    model_path = ensure_whisper_models(context, params, requested_model)
    model_dir = os.path.dirname(model_path)

    audio_file = os.path.expanduser(LaunchConfiguration("audio_file").perform(context))
    if not audio_file:
        raise RuntimeError(
            "audio_file launch argument is required, e.g. "
            "`audio_file:=/root/topsbot/models/whisper_small/output.wav`")
    start_delay = float(params.get(
        "audio.start_delay_seconds", 5.0))
    topic = LaunchConfiguration("input_topic").perform(context) or str(
        params.get("input_topic", "/audio_raw"))

    overrides = {"model.path": model_path}
    for launch_name, parameter_name in (
        ("input_topic", "input_topic"),
        ("text_topic", "text_topic"),
    ):
        value = LaunchConfiguration(launch_name).perform(context)
        if value:
            overrides[parameter_name] = value

    return [
        Node(
            package="topsbot_whisper_asr",
            executable="audio_source_node.py",
            name="whisper_audio_source",
            output="screen",
            arguments=[audio_file, "--topic", topic, "--start-delay", str(start_delay)],
            parameters=[{
                "sample_rate": int(params.get("audio.sample_rate", 16000)),
                "channels": int(params.get("audio.channels", 1)),
            }],
        ),
        Node(
            package="topsbot_whisper_asr",
            executable="whisper_asr_node",
            name="whisper_asr",
            output="screen",
            parameters=[params_path, overrides],
            cwd=model_dir,
        ),
    ]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("params_file", default_value="whisper.yaml"),
        DeclareLaunchArgument(
            "audio_file",
            default_value="",
            description="WAV or s16le PCM to stream into /audio_raw (required)",
        ),
        DeclareLaunchArgument("input_topic", default_value=""),
        DeclareLaunchArgument("text_topic", default_value=""),
        DeclareLaunchArgument("model_path", default_value=""),
        DeclareLaunchArgument("models_dir", default_value="~/topsbot/models/whisper_small"),
        DeclareLaunchArgument("auto_download_models", default_value="true"),
        OpaqueFunction(function=_launch_setup),
    ])