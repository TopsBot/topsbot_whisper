# Copyright 2026 TOPSBOT
# SPDX-License-Identifier: Apache-2.0

import os
import sys

from ament_index_python.packages import get_package_prefix, get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, ExecuteProcess, LogInfo, OpaqueFunction
from launch.substitutions import LaunchConfiguration

_launch_dir = os.path.dirname(os.path.abspath(__file__))
if _launch_dir not in sys.path:
    sys.path.insert(0, _launch_dir)
from model_utils import (  # noqa: E402
    ensure_whisper_models,
    load_params,
    param_value,
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

    pcm_file = os.path.expanduser(LaunchConfiguration("pcm_file").perform(context))

    task = str(param_value(params, "asr.task", "transcribe"))
    language = str(param_value(params, "asr.language", ""))
    prompt = str(param_value(params, "asr.prompt", ""))

    executable = os.path.join(
        get_package_prefix("topsbot_whisper_asr"),
        "lib", "topsbot_whisper_asr", "whisper_file_cli")
    command = [
        executable,
        pcm_file,
        "--model-path", model_path,
        "--task", task,
        "--language", language,
        "--prompt", prompt,
    ]
    return [
        LogInfo(msg=f"[file_whisper] params_file: {params_path}"),
        LogInfo(msg=f"[file_whisper] transcribing: {pcm_file}"),
        ExecuteProcess(cmd=command, output="screen", cwd=model_dir),
    ]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument("params_file", default_value="file_whisper.yaml"),
        DeclareLaunchArgument(
            "pcm_file",
            default_value="",
            description="Input s16le PCM file (required)",
        ),
        DeclareLaunchArgument("model_path", default_value=""),
        DeclareLaunchArgument("models_dir", default_value="~/topsbot/models/whisper_small"),
        DeclareLaunchArgument("auto_download_models", default_value="true"),
        OpaqueFunction(function=_launch_setup),
    ])