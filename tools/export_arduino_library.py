#!/usr/bin/env python3
# File: tools/export_arduino_library.py
"""Packages Pigweed AVR port into a standalone Arduino IDE / Arduino CLI library."""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PW_ROOT = REPO_ROOT / "third_party" / "pigweed"
STDCPP_ROOT = REPO_ROOT / "third_party" / "avr-libstdcpp" / "include"


def generate_pw_rpc_packet_header(output_dir: Path) -> Path:
    """Generate Pigweed's PWPB RPC packet header into an export directory."""
    output_dir = output_dir.resolve()
    proto_path = PW_ROOT / "pw_rpc" / "internal" / "packet.proto"
    plugin_path = PW_ROOT / "pw_protobuf" / "py" / "pw_protobuf" / "plugin.py"
    protoc = shutil.which("protoc")
    if not proto_path.is_file() or not plugin_path.is_file():
        raise FileNotFoundError(
            "Pigweed pw_rpc packet.proto or PWPB plugin is missing; "
            "initialize the Pigweed submodule recursively"
        )
    if protoc is None:
        raise FileNotFoundError(
            "protoc is required to export upstream pw_rpc; install protobuf-compiler"
        )

    protobuf_proto_dir = PW_ROOT / "pw_protobuf" / "pw_protobuf_protos"
    codegen_proto_dir = PW_ROOT / "pw_protobuf" / "pw_protobuf_codegen_protos"
    required_protos = (
        protobuf_proto_dir / "common.proto",
        protobuf_proto_dir / "field_options.proto",
        protobuf_proto_dir / "status.proto",
        codegen_proto_dir / "codegen_options.proto",
    )
    missing_protos = [path for path in required_protos if not path.is_file()]
    if missing_protos:
        raise FileNotFoundError(
            "Pigweed Python code-generation protos are missing: "
            + ", ".join(str(path) for path in missing_protos)
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pigweed-pwpb-codegen-") as tmp:
        generated_python_root = Path(tmp) / "py"
        protobuf_python_dir = generated_python_root / "pw_protobuf_protos"
        codegen_python_dir = generated_python_root / "pw_protobuf_codegen_protos"
        rpc_internal_python_dir = generated_python_root / "pw_rpc" / "internal"
        protobuf_python_dir.mkdir(parents=True)
        codegen_python_dir.mkdir(parents=True)
        rpc_internal_python_dir.mkdir(parents=True)

        for package_dir in (
            protobuf_python_dir,
            codegen_python_dir,
            rpc_internal_python_dir,
        ):
            (package_dir / "__init__.py").write_text("", encoding="utf-8")
        (generated_python_root / "pw_rpc" / "__init__.py").write_text(
            "from pkgutil import extend_path\n__path__ = extend_path(__path__, __name__)\n",
            encoding="utf-8",
        )

        generation_commands = (
            (
                protobuf_proto_dir,
                f"-I{protobuf_proto_dir}",
                f"--python_out={protobuf_python_dir}",
                "common.proto",
                "field_options.proto",
                "status.proto",
            ),
            (
                codegen_proto_dir,
                f"-I{codegen_proto_dir}",
                f"--python_out={codegen_python_dir}",
                "codegen_options.proto",
            ),
            (
                PW_ROOT,
                f"-I{PW_ROOT}",
                f"--python_out={generated_python_root}",
                "pw_rpc/internal/packet.proto",
            ),
        )
        for work_dir, *arguments in generation_commands:
            try:
                subprocess.run(
                    [protoc, "--experimental_allow_proto3_optional", *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                    cwd=work_dir,
                )
            except subprocess.CalledProcessError as exc:
                raise RuntimeError(
                    "Pigweed Python code-generation proto compilation failed:\n"
                    f"{exc.stderr or exc.stdout}"
                ) from exc

        python_paths = (
            generated_python_root,
            PW_ROOT / "pw_status" / "py",
            PW_ROOT / "pw_protobuf" / "py",
            PW_ROOT / "pw_protobuf_compiler" / "py",
            PW_ROOT / "pw_rpc" / "py",
            PW_ROOT / "pw_codegen" / "py",
            PW_ROOT / "pw_cli" / "py",
        )
        env = os.environ.copy()
        existing_pythonpath = env.get("PYTHONPATH")
        python_path_entries = [str(path) for path in python_paths]
        if existing_pythonpath:
            python_path_entries.append(existing_pythonpath)
        env["PYTHONPATH"] = os.pathsep.join(python_path_entries)

        command = [
            protoc,
            "--experimental_allow_proto3_optional",
            f"-I{PW_ROOT}",
            f"--plugin=protoc-gen-pwpb={plugin_path}",
            f"--pwpb_out={output_dir}",
            "pw_rpc/internal/packet.proto",
        ]
        try:
            subprocess.run(
                command,
                check=True,
                capture_output=True,
                text=True,
                cwd=PW_ROOT,
                env=env,
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                "Pigweed pw_rpc PWPB packet generation failed:\n"
                f"{exc.stderr or exc.stdout}"
            ) from exc

        raw_rpc_plugin_path = PW_ROOT / "pw_rpc" / "py" / "pw_rpc" / "plugin_raw.py"
        transfer_proto_dir = PW_ROOT / "pw_transfer"
        transfer_out_dir = output_dir / "pw_transfer"
        transfer_out_dir.mkdir(parents=True, exist_ok=True)
        transfer_command = [
            protoc,
            "--experimental_allow_proto3_optional",
            f"-I{transfer_proto_dir}",
            f"--plugin=protoc-gen-pwpb={plugin_path}",
            f"--pwpb_opt=-I{transfer_proto_dir}",
            f"--pwpb_out={transfer_out_dir}",
            f"--plugin=protoc-gen-raw_rpc={raw_rpc_plugin_path}",
            f"--raw_rpc_out={transfer_out_dir}",
            "transfer.proto",
        ]
        try:
            subprocess.run(
                transfer_command,
                check=True,
                capture_output=True,
                text=True,
                cwd=transfer_proto_dir,
                env=env,
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                "Pigweed pw_transfer code generation failed:\n"
                f"{exc.stderr or exc.stdout}"
            ) from exc

    packet_header = output_dir / "pw_rpc" / "internal" / "packet.pwpb.h"
    if not packet_header.is_file():
        raise FileNotFoundError(
            f"PWPB generator did not create the expected header: {packet_header}"
        )
    transfer_header = output_dir / "pw_transfer" / "transfer.pwpb.h"
    if not transfer_header.is_file():
        raise FileNotFoundError(
            f"PWPB generator did not create the expected header: {transfer_header}"
        )
    return packet_header


def export_library(dest_dir: Path):
    # CMake probes and Arduino packaging are separate CI steps; ensure the
    # exact pinned upstream Pigweed source has the reviewed AVR patch applied.
    patch_script = REPO_ROOT / "tools" / "ci" / "apply_pigweed_patches.py"
    subprocess.run(
        [sys.executable, str(patch_script)],
        check=True,
        cwd=REPO_ROOT,
    )

    print(f"[*] Exporting Pigweed AVR Arduino Library to: {dest_dir}")
    if dest_dir.exists():
        shutil.rmtree(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)

    src_dir = dest_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)

    # 1. Copy library.properties
    shutil.copy2(REPO_ROOT / "library.properties", dest_dir / "library.properties")

    # 2. Copy root header and minimal runtime stubs
    for f in [
        "PigweedAVR.h",
        "arduino_stream.h",
        "avr_stdio_compat.h",
        "cxx_abi_stubs.cpp",
    ]:
        shutil.copy2(REPO_ROOT / "src" / f, src_dir / f)

    # 3. Copy custom AVR backends
    backends = [
        "pw_assert_avr",
        "pw_chrono_avr",
        "pw_log_avr",
    ]
    for backend in backends:
        b_path = REPO_ROOT / backend
        if (b_path / "public").exists():
            shutil.copytree(b_path / "public", src_dir, dirs_exist_ok=True)
        if (b_path / "public_overrides").exists():
            shutil.copytree(b_path / "public_overrides", src_dir, dirs_exist_ok=True)
        if (b_path / "src").exists():
            for src_file in (b_path / "src").glob("*.cpp"):
                shutil.copy2(src_file, src_dir / src_file.name)

    # 4. Copy Pigweed modules public headers and implementation sources
    modules = [
        "pw_status",
        "pw_result",
        "pw_span",
        "pw_bytes",
        "pw_varint",
        "pw_containers",
        "pw_stream",
        "pw_checksum",
        "pw_hdlc",
        "pw_function",
        "pw_string",
        "pw_base64",
        "pw_tokenizer",
        "pw_chrono",
        "pw_sync",
        "pw_preprocessor",
        "pw_numeric",
        "pw_enum",
        "pw_memory",
        "pw_polyfill",
        "pw_protobuf",
        "pw_assert",
        "pw_rpc",
        "pw_transfer",
        "pw_log",
        "pw_toolchain",
    ]

    for mod in modules:
        mod_path = PW_ROOT / mod
        pub_dirs = [mod_path / "public", mod_path / "cpp" / "public"]
        for p in pub_dirs:
            if p.exists():
                shutil.copytree(p, src_dir, dirs_exist_ok=True)

        if mod == "pw_sync":
            continue

        EXCLUDED_SOURCES = {
            "pw_assert_examples.cc",
            "pw_assert_fake_backend.cc",
            "pw_polyfill_test.cc",
            "pw_containers_test_helpers.cc",
            "pw_tokenizer_generate_decoding_test_data.cc",
            "pw_tokenizer_tokenize_c99_test_entry_point.cc",
            "pw_tokenizer_base64.cc",
            "pw_tokenizer_csv.cc",
            "pw_tokenizer_detokenize.cc",
            "pw_tokenizer_detokenize_from_this_program.cc",
            "pw_tokenizer_token_database.cc",
            "pw_tokenizer_decode.cc",
            "pw_containers_aa_tree.cc",
            "pw_containers_aa_tree_item.cc",
            "pw_containers_intrusive_map.cc",
            "pw_stream_socket_stream.cc",
            "pw_stream_std_file_stream.cc",
            "pw_hdlc_hdlc_sys_io_system_server.cc",
            "pw_hdlc_router.cc",
            "pw_hdlc_wire_packet_parser.cc",
            "pw_chrono_wrap_time_build_time.cc",
            "pw_chrono_wrap_time_system_clock.cc",
            "pw_chrono_system_clock.cc",
            "pw_log_proto_utils.cc",
            "pw_crypto_aes_boringssl.cc",
            "pw_crypto_aes_mbedtls.cc",
            "pw_crypto_chacha20_boringssl.cc",
            "pw_crypto_chacha20_mbedtls.cc",
            "pw_crypto_ecdh_boringssl.cc",
            "pw_crypto_ecdh_mbedtls.cc",
            "pw_crypto_ecdsa_mbedtls.cc",
            "pw_crypto_sha256_mbedtls.cc",
            "pw_crypto_sha256_mock.cc",
            "pw_ring_buffer_prefixed_entry_ring_buffer.cc",
            "pw_string_string_builder.cc",
            "pw_string_type_to_string.cc",
            "pw_string_format.cc",
            "pw_string_utf_codecs.cc",
            "pw_bytes_byte_builder.cc",
            "pw_bytes_alignment.cc",
            "pw_containers_inline_var_len_entry_queue.cc",
            "pw_containers_intrusive_queue.cc",
            "pw_containers_var_len_entry.cc",
            "pw_containers_var_len_entry_queue_iterator.cc",
            "pw_transfer_atomic_file_transfer_handler.cc",
        # The Bridge uses fixed-lifetime stream handlers, not allocator-backed
        # file handlers; excluding this unused source avoids pulling in a
        # platform mutex backend solely for an unused allocation API.
        "pw_transfer_handler_allocator.cc",
        }

        # Copy .cc sources for compilation
        for cc_file in mod_path.glob("*.cc"):
            if mod == "pw_rpc" and cc_file.name not in {
                "call.cc",
                "client.cc",
                "client_call.cc",
                "channel.cc",
                "channel_list.cc",
                "endpoint.cc",
                "packet.cc",
                "packet_meta.cc",
                "server.cc",
                "server_call.cc",
                "service.cc",
            }:
                continue
            target_name = f"{mod}_{cc_file.name}"
            if (
                not cc_file.name.endswith(("_test.cc", "_fuzzer.cc", "_perf_test.cc"))
                and cc_file.name not in EXCLUDED_SOURCES
                and target_name not in EXCLUDED_SOURCES
            ):
                shutil.copy2(cc_file, src_dir / target_name)
        for cc_file in (mod_path / "cpp").glob("*.cc"):
            target_name = f"{mod}_{cc_file.name}"
            if (
                not cc_file.name.endswith(("_test.cc", "_fuzzer.cc"))
                and cc_file.name not in EXCLUDED_SOURCES
                and target_name not in EXCLUDED_SOURCES
            ):
                shutil.copy2(cc_file, src_dir / target_name)

    nanopb_rpc_path = PW_ROOT / "pw_rpc" / "nanopb"
    shutil.copytree(nanopb_rpc_path / "public", src_dir, dirs_exist_ok=True)
    for cc_file in nanopb_rpc_path.glob("*.cc"):
        if cc_file.name in {"common.cc", "method.cc", "server_reader_writer.cc"}:
            shutil.copy2(cc_file, src_dir / f"pw_rpc_nanopb_{cc_file.name}")
    raw_rpc_path = PW_ROOT / "pw_rpc" / "raw"
    shutil.copytree(raw_rpc_path / "public", src_dir, dirs_exist_ok=True)
    shutil.copy2(raw_rpc_path / "method.cc", src_dir / "pw_rpc_raw_method.cc")

    # Upstream module headers are copied after the first backend pass above.
    # Re-apply the selected AVR backend headers last so Pigweed's generic
    # pw_assert_backend templates cannot overwrite the AVR ABI/macros.
    for backend in ("pw_assert_avr", "pw_chrono_avr", "pw_log_avr"):
        backend_public = REPO_ROOT / backend / "public"
        if backend_public.is_dir():
            shutil.copytree(backend_public, src_dir, dirs_exist_ok=True)
        backend_overrides = REPO_ROOT / backend / "public_overrides"
        if backend_overrides.is_dir():
            shutil.copytree(backend_overrides, src_dir, dirs_exist_ok=True)

    generate_pw_rpc_packet_header(src_dir)

    # pw_transfer's protocol bindings are generated by the CMake build. Search
    # both build trees because the AVR transfer probe is configured separately
    # from the main ATmega2560 build.
    transfer_thread_header = (
        PW_ROOT / "pw_transfer" / "public" / "pw_transfer" / "transfer_thread.h"
    )
    transfer_thread_source = PW_ROOT / "pw_transfer" / "transfer_thread.cc"
    if (
        not transfer_thread_header.is_file()
        or "Poll(size_t budget" not in transfer_thread_header.read_text(encoding="utf-8")
        or not transfer_thread_source.is_file()
        or "TransferThread::Poll(size_t budget)" not in transfer_thread_source.read_text(encoding="utf-8")
    ):
        raise RuntimeError(
            "pw_transfer cooperative source patch is missing. Run CMake configure "
            "with the AVR port before exporting the Arduino library."
        )

    # The Bridge uses the upstream stream-backed handlers and does not
    # expose the allocator-backed file-handler API.
    (src_dir / "pw_transfer" / "handler_allocator.h").unlink(missing_ok=True)

    # Fuchsia stdcompat and fit dependencies (required by pw_function)
    for fuchsia_lib in ["stdcompat", "fit"]:
        fuchsia_include = (
            PW_ROOT
            / "third_party"
            / "fuchsia"
            / "repo"
            / "sdk"
            / "lib"
            / fuchsia_lib
            / "include"
        )
        if fuchsia_include.exists():
            shutil.copytree(fuchsia_include, src_dir, dirs_exist_ok=True)

    # Copy avr-libstdcpp versioned trees
    for gcc_ver in ["gcc10", "gcc12", "gcc13", "gcc15", "gcc16"]:
        gcc_path = REPO_ROOT / "third_party" / "avr-libstdcpp" / gcc_ver
        if gcc_path.exists():
            shutil.copytree(gcc_path, dest_dir / gcc_ver, dirs_exist_ok=True)

    # Copy avr-libstdcpp headers directly for AVR targets
    if STDCPP_ROOT.exists():
        shutil.copytree(STDCPP_ROOT, src_dir, dirs_exist_ok=True)

    # 5. Create an upstream pw_rpc + HDLC server skeleton.
    # Applications register their generated Pigweed services on this server.
    examples_dir = dest_dir / "examples" / "YunBridgeRpc"
    examples_dir.mkdir(parents=True, exist_ok=True)

    example_ino = """// Arduino Yún: upstream Pigweed pw_rpc over HDLC/UART.
#include <Arduino.h>
#include <array>
#include <PigweedAVR.h>
#include "avr_uart_stream.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/server.h"

namespace {
constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;

avr_port::AvrUartStream uart(115200, avr_port::UartPort::kUsart1);
pw::hdlc::RpcChannelOutput output(uart, kHdlcAddress, "yun-rpc");
std::array<pw::rpc::Channel, 1> channels = {
    pw::rpc::Channel::Create<kRpcChannelId>(&output)};
pw::rpc::Server server(pw::span<pw::rpc::Channel>(channels.data(),
                                                   channels.size()));
std::byte decoder_storage[128]{};
pw::hdlc::Decoder decoder{pw::ByteSpan(decoder_storage)};

void PollUart() {
  while (Serial1.available() > 0) {
    const std::byte value{static_cast<uint8_t>(Serial1.read())};
    auto frame = decoder.Process(value);
    if (!frame.ok()) continue;
    if (frame->address() != kHdlcAddress) continue;
    (void)server.ProcessPacket(frame->data());
  }
}
}  // namespace

void setup() {
  Serial1.begin(115200);
  // Register generated pw_rpc services here, before processing packets.
}

void loop() {
  PollUart();
}
"""
    (examples_dir / "YunBridgeRpc.ino").write_text(example_ino)

    print(
        f"[+] Successfully exported Arduino Library ({len(list(src_dir.glob('*')))} components)."
    )


if __name__ == "__main__":
    out = (
        Path(sys.argv[1]) if len(sys.argv) > 1 else (REPO_ROOT / "dist" / "PigweedAVR")
    )
    export_library(out)
