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
        protobuf_python_dir.mkdir(parents=True)
        codegen_python_dir.mkdir(parents=True)

        for package_dir in (protobuf_python_dir, codegen_python_dir):
            (package_dir / "__init__.py").write_text("", encoding="utf-8")

        generation_commands = (
            (
                f"-I{protobuf_proto_dir}",
                f"--python_out={protobuf_python_dir}",
                "common.proto",
                "field_options.proto",
                "status.proto",
            ),
            (
                f"-I{codegen_proto_dir}",
                f"--python_out={codegen_python_dir}",
                "codegen_options.proto",
            ),
        )
        for arguments in generation_commands:
            try:
                subprocess.run(
                    [protoc, "--experimental_allow_proto3_optional", *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                    cwd=protobuf_proto_dir if "common.proto" in arguments else codegen_proto_dir,
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

    packet_header = output_dir / "pw_rpc" / "internal" / "packet.pwpb.h"
    if not packet_header.is_file():
        raise FileNotFoundError(
            f"PWPB generator did not create the expected header: {packet_header}"
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
            "pw_log_rate_limited.cc",
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
                and target_name not in EXCLUDED_SOURCES
            ):
                shutil.copy2(cc_file, src_dir / target_name)
        for cc_file in (mod_path / "cpp").glob("*.cc"):
            target_name = f"{mod}_{cc_file.name}"
            if (
                not cc_file.name.endswith(("_test.cc", "_fuzzer.cc"))
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

    generate_pw_rpc_packet_header(src_dir)

    # pw_transfer's protocol bindings are generated by the CMake build. Search
    # both build trees because the AVR transfer probe is configured separately
    # from the main ATmega328P build.
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

    generated_transfer_packages = {
        "pwpb": "transfer.pwpb.h",
        "raw_rpc": "transfer.raw_rpc.pb.h",
    }
    generated_transfer_root = None
    for build_root in (REPO_ROOT / "build-transfer-1284p", REPO_ROOT / "build-transfer", REPO_ROOT / "build"):
        if not build_root.is_dir():
            continue
        for proto_root in build_root.rglob("pw_transfer.proto"):
            if all(
                (proto_root / generator / "pw_transfer" / header).is_file()
                for generator, header in generated_transfer_packages.items()
            ):
                generated_transfer_root = proto_root
                break
        if generated_transfer_root is not None:
            break
    if generated_transfer_root is None:
        raise FileNotFoundError(
            "pw_transfer generated PWPB/raw RPC headers are missing. "
            "Configure and build the pw_transfer AVR probe before exporting."
        )

    for generator in generated_transfer_packages:
        generated_package = generated_transfer_root / generator / "pw_transfer"
        shutil.copytree(
            generated_package,
            src_dir / "pw_transfer",
            dirs_exist_ok=True,
        )

    cooperative_transfer_defines = (
        "#ifndef PW_TRANSFER_COOPERATIVE\n"
        "#define PW_TRANSFER_COOPERATIVE 1\n"
        "#endif\n"
        "#ifndef PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY\n"
        "#define PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY 2\n"
        "#endif\n"
        "#ifndef PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE\n"
        "#define PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE 128\n"
        "#endif\n"
    )
    for relative_path in (
        "pw_transfer/transfer.h",
        "pw_transfer/transfer_thread.h",
    ):
        path = src_dir / relative_path
        content = path.read_text(encoding="utf-8")
        if "define PW_TRANSFER_COOPERATIVE 1" not in content:
            if "#pragma once\n" not in content:
                raise RuntimeError(
                    f"Cannot enable cooperative pw_transfer in {relative_path}: "
                    "missing #pragma once"
                )
            content = content.replace(
                "#pragma once\n",
                "#pragma once\n\n" + cooperative_transfer_defines,
                1,
            )
            path.write_text(content, encoding="utf-8")

    rpc_config = src_dir / "pw_rpc" / "internal" / "config.h"
    if not rpc_config.is_file():
        raise FileNotFoundError(f"Missing upstream pw_rpc configuration: {rpc_config}")

    content = rpc_config.read_text(encoding="utf-8")
    for original, replacement in (
        (
            "#define PW_RPC_USE_GLOBAL_MUTEX 1",
            "#define PW_RPC_USE_GLOBAL_MUTEX 0",
        ),
        (
            "#define PW_RPC_ENCODING_BUFFER_SIZE_BYTES 512",
            "#define PW_RPC_ENCODING_BUFFER_SIZE_BYTES 256",
        ),
    ):
        if original not in content:
            raise RuntimeError(
                f"Upstream pw_rpc configuration no longer matches the AVR patch: {original}"
            )
        content = content.replace(original, replacement, 1)
    rpc_config.write_text(content, encoding="utf-8")

    def replace_in_export(relative_path: str, old: str, new: str) -> None:
        path = src_dir / relative_path
        content = path.read_text(encoding="utf-8")
        if new in content:
            return
        if old not in content:
            # The source-tree AVR patch already provides these double guards.
            # The Arduino export must not try to apply a second, older variant.
            if (
                relative_path.startswith("pw_protobuf/")
                and "Double" in old
                and "if constexpr (sizeof(double) != sizeof(uint64_t))" in content
            ):
                return
            raise RuntimeError(
                f"Pigweed compatibility patch did not match {relative_path}"
            )
        path.write_text(content.replace(old, new, 1), encoding="utf-8")

    replace_in_export(
        "pw_checksum/internal/config.h",
        "#ifndef PW_CHECKSUM_CRC32_DEFAULT_IMPL\n"
        "#define PW_CHECKSUM_CRC32_DEFAULT_IMPL PW_CHECKSUM_CRC32_8BITS\n"
        "#endif  // PW_CHECKSUM_CRC32_DEFAULT_IMPL",
        "#ifndef PW_CHECKSUM_CRC32_DEFAULT_IMPL\n"
        "#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)\n"
        "#define PW_CHECKSUM_CRC32_DEFAULT_IMPL PW_CHECKSUM_CRC32_4BITS\n"
        "#else\n"
        "#define PW_CHECKSUM_CRC32_DEFAULT_IMPL PW_CHECKSUM_CRC32_8BITS\n"
        "#endif\n"
        "#endif  // PW_CHECKSUM_CRC32_DEFAULT_IMPL",
    )

    replace_in_export(
        "pw_protobuf/wire_format.h",
        "kMaxFieldNumber = (1u << 29) - 1;",
        "kMaxFieldNumber = (uint32_t{1} << 29) - 1;",
    )
    replace_in_export(
        "pw_protobuf/internal/codegen.h",
        "static constexpr unsigned int kMaxFieldSize = (1u << 16) - 1;",
        "static constexpr uint32_t kMaxFieldSize = (uint32_t{1} << 16) - 1;",
    )
    codegen_header = src_dir / "pw_protobuf/internal/codegen.h"
    if "MessageField should be four 32-bit words or less on 16-bit targets" not in codegen_header.read_text(encoding="utf-8"):
        replace_in_export(
            "pw_protobuf/internal/codegen.h",
            "static_assert(sizeof(MessageField) <= sizeof(size_t) * 4,\n"
            '              "MessageField should be four words or less");',
            "static_assert(sizeof(MessageField) <=\n"
            "                  (sizeof(size_t) == 2 ? 12 : sizeof(size_t) * 4),\n"
            '              "MessageField exceeds the target-specific size budget");',
        )

    encoder_path = "pw_protobuf/encoder.h"
    replace_in_export(
        encoder_path,
        '#include "pw_memory/internal/sibling_cast.h"',
        '#include "pw_memory/internal/sibling_cast.h"\n'
        '#include "pw_polyfill/language_feature_macros.h"',
    )
    replace_in_export(
        encoder_path,
        "constexpr StreamEncoder(stream::Writer& writer, ByteSpan scratch_buffer)",
        "PW_CONSTEXPR_CPP20 StreamEncoder(stream::Writer& writer,\n"
        "                                   ByteSpan scratch_buffer)",
    )
    replace_in_export(
        encoder_path,
        "constexpr explicit StreamEncoder(ByteSpan dest)",
        "PW_CONSTEXPR_CPP20 explicit StreamEncoder(ByteSpan dest)",
    )
    replace_in_export(
        encoder_path,
        "constexpr StreamEncoder(StreamEncoder& parent,\n"
        "                          ByteSpan scratch_buffer,\n"
        "                          bool write_when_empty = true)",
        "PW_CONSTEXPR_CPP20 StreamEncoder(StreamEncoder& parent,\n"
        "                                   ByteSpan scratch_buffer,\n"
        "                                   bool write_when_empty = true)",
    )
    replace_in_export(
        encoder_path,
        "constexpr MemoryEncoder(ByteSpan dest) : StreamEncoder(dest) {}",
        "PW_CONSTEXPR_CPP20 MemoryEncoder(ByteSpan dest) : StreamEncoder(dest) {}",
    )
    replace_in_export(
        encoder_path,
        "  Status WriteDouble(uint32_t field_number, double value) {\n"
        "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
        '                  "Double and uint64_t are not the same size");\n'
        "    uint64_t integral_value;\n"
        "    std::memcpy(&integral_value, &value, sizeof(value));\n"
        "    std::array<std::byte, sizeof(value)> data =\n"
        "        bytes::CopyInOrder(endian::little, integral_value);\n"
        "    return WriteFixed(field_number, data);\n"
        "  }",
        "  Status WriteDouble(uint32_t field_number, double value) {\n"
        "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
        "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
        '                  "Double and uint64_t are not the same size");\n'
        "    uint64_t integral_value;\n"
        "    std::memcpy(&integral_value, &value, sizeof(value));\n"
        "    std::array<std::byte, sizeof(value)> data =\n"
        "        bytes::CopyInOrder(endian::little, integral_value);\n"
        "    return WriteFixed(field_number, data);\n"
        "#else\n"
        "    (void)field_number;\n"
        "    (void)value;\n"
        "    return Status::Unimplemented();\n"
        "#endif\n"
        "  }",
    )
    replace_in_export(
        encoder_path,
        "  Status WritePackedDouble(uint32_t field_number, span<const double> values) {\n"
        "    return WritePackedFixed(field_number, as_bytes(values), sizeof(double));\n"
        "  }",
        "  Status WritePackedDouble(uint32_t field_number, span<const double> values) {\n"
        "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
        "    return WritePackedFixed(field_number, as_bytes(values), sizeof(double));\n"
        "#else\n"
        "    (void)field_number;\n"
        "    (void)values;\n"
        "    return Status::Unimplemented();\n"
        "#endif\n"
        "  }",
    )
    replace_in_export(
        encoder_path,
        "  Status WriteRepeatedDouble(uint32_t field_number,\n"
        "                             const pw::Vector<double>& values) {\n"
        "    return WritePackedFixed(field_number,\n"
        "                            as_bytes(span(values.data(), values.size())),\n"
        "                            sizeof(double));\n"
        "  }",
        "  Status WriteRepeatedDouble(uint32_t field_number,\n"
        "                             const pw::Vector<double>& values) {\n"
        "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
        "    return WritePackedFixed(field_number,\n"
        "                            as_bytes(span(values.data(), values.size())),\n"
        "                            sizeof(double));\n"
        "#else\n"
        "    (void)field_number;\n"
        "    (void)values;\n"
        "    return Status::Unimplemented();\n"
        "#endif\n"
        "  }",
    )

    for decoder_path, old, new in [
        (
            "pw_protobuf/decoder.h",
            "  Status ReadDouble(double* out) {\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadFixed(out);\n"
            "  }",
            "  Status ReadDouble(double* out) {\n"
            "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadFixed(out);\n"
            "#else\n"
            "    (void)out;\n"
            "    return Status::Unimplemented();\n"
            "#endif\n"
            "  }",
        ),
        (
            "pw_protobuf/stream_decoder.h",
            "  Result<double> ReadDouble() {\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadFixedField<double>();\n"
            "  }",
            "  Result<double> ReadDouble() {\n"
            "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadFixedField<double>();\n"
            "#else\n"
            "    return Status::Unimplemented();\n"
            "#endif\n"
            "  }",
        ),
        (
            "pw_protobuf/stream_decoder.h",
            "  StatusWithSize ReadPackedDouble(span<double> out) {\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadPackedFixedField(as_writable_bytes(out), sizeof(double));\n"
            "  }",
            "  StatusWithSize ReadPackedDouble(span<double> out) {\n"
            "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
            "    static_assert(sizeof(double) == sizeof(uint64_t),\n"
            '                  "Double and uint64_t must be the same size for protobufs");\n'
            "    return ReadPackedFixedField(as_writable_bytes(out), sizeof(double));\n"
            "#else\n"
            "    (void)out;\n"
            "    return StatusWithSize::Unimplemented();\n"
            "#endif\n"
            "  }",
        ),
        (
            "pw_protobuf/stream_decoder.h",
            "  Status ReadRepeatedDouble(pw::Vector<double>& out) {\n"
            "    return ReadRepeatedFixedField<double>(out);\n"
            "  }",
            "  Status ReadRepeatedDouble(pw::Vector<double>& out) {\n"
            "#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8\n"
            "    return ReadRepeatedFixedField<double>(out);\n"
            "#else\n"
            "    (void)out;\n"
            "    return Status::Unimplemented();\n"
            "#endif\n"
            "  }",
        ),
    ]:
        replace_in_export(decoder_path, old, new)

    replace_in_export(
        "pw_stream/null_stream.h",
        "    PW_CONSTINIT static NullStream stream;",
        "#if (defined(__cpp_constinit) && __cpp_constinit >= 201907L) || \\\n"
        "    defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 10)\n"
        "    PW_CONSTINIT static NullStream stream;\n"
        "#else\n"
        "    static NullStream stream;\n"
        "#endif",
    )
    replace_in_export(
        "pw_containers/internal/intrusive_list_item.h",
        "template <typename>\n" "class GenericIntrusiveList;\n",
        "template <typename>\n"
        "class GenericIntrusiveList;\n\n"
        "template <typename, typename>\n"
        "class Incrementable;\n",
    )
    replace_in_export(
        "pw_rpc/internal/call.h",
        "        proto_on_next_local = std::move(proto_on_next);\n\n"
        "        rpc_lock().unlock();\n"
        "        proto_on_next_local(proto_struct);\n"
        "        rpc_lock().lock();\n\n"
        "        // Restore the original callback if the original call is still active\n"
        "        // and the callback has not been replaced.\n"
        "        // NOLINTNEXTLINE(bugprone-use-after-move)\n"
        "        const bool callback_replaced = proto_on_next != nullptr;",
        "        proto_on_next_local = std::exchange(proto_on_next, nullptr);\n\n"
        "        rpc_lock().unlock();\n"
        "        proto_on_next_local(proto_struct);\n"
        "        rpc_lock().lock();\n\n"
        "        // Restore the original callback if the original call is still active\n"
        "        // and the callback has not been replaced.\n"
        "        const bool callback_replaced = proto_on_next != nullptr;",
    )
    replace_in_export(
        "pw_rpc_packet.cc",
        "namespace RpcPacket = pwpb::RpcPacket;",
        "namespace RpcPacketProto = ::pw::rpc::internal::pwpb::RpcPacket;",
    )
    packet_source = src_dir / "pw_rpc_packet.cc"
    packet_content = packet_source.read_text(encoding="utf-8")
    packet_source.write_text(
        packet_content.replace("RpcPacket::", "RpcPacketProto::"),
        encoding="utf-8",
    )

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

    # 4.1 Patch pw_tokenizer for 16-bit integer architectures (AVR: sizeof(int) == 2, sizeof(long) == 4)
    arg_types_file = src_dir / "pw_tokenizer" / "internal" / "argument_types.h"
    if arg_types_file.exists():
        content = arg_types_file.read_text()
        if "sizeof(ArgType) == sizeof(int64_t)" in content:
            content = content.replace(
                "} else if constexpr (sizeof(ArgType) == sizeof(int64_t)) {",
                "} else if constexpr (sizeof(ArgType) > sizeof(int)) {\n    static_assert(sizeof(ArgType) <= sizeof(int64_t));",
            )
            arg_types_file.write_text(content)

    # 4.2 Patch pw_containers/vector.h for GCC 7 compatibility (defaulted constructor noexcept mismatch)
    vec_file = src_dir / "pw_containers" / "vector.h"
    if vec_file.exists():
        c = vec_file.read_text(encoding="utf-8")
        c = c.replace("Vector() noexcept = default;", "Vector() = default;")
        c = c.replace(
            "VectorStorage() : Base(kMaxSize) {}",
            "VectorStorage() noexcept : Base(kMaxSize) {}",
        )
        vec_file.write_text(c, encoding="utf-8")

    # 4.3 Patch pw_tokenizer/encode_args.h for GCC 7 (non-constant static_assert condition)
    enc_args = src_dir / "pw_tokenizer" / "encode_args.h"
    if enc_args.exists():
        c = enc_args.read_text(encoding="utf-8")
        c = c.replace(
            "static_assert(token_bytes.size() == sizeof(pw_tokenizer_Token));",
            "static_assert(sizeof(token_bytes) == sizeof(pw_tokenizer_Token));",
        )
        enc_args.write_text(c, encoding="utf-8")

    # 4.4 Patch pw_stream_memory_stream.cc for Arduino min macro collision
    mem_stream = src_dir / "pw_stream_memory_stream.cc"
    if mem_stream.exists():
        c = mem_stream.read_text(encoding="utf-8")
        if "#include <algorithm>" not in c:
            c = "#include <algorithm>\n#ifdef min\n#undef min\n#endif\n" + c
            mem_stream.write_text(c, encoding="utf-8")

    # Copy avr-libstdcpp versioned trees
    for gcc_ver in ["gcc10", "gcc12", "gcc13", "gcc15", "gcc16"]:
        gcc_path = REPO_ROOT / "third_party" / "avr-libstdcpp" / gcc_ver
        if gcc_path.exists():
            shutil.copytree(gcc_path, dest_dir / gcc_ver, dirs_exist_ok=True)

    # Copy avr-libstdcpp headers with architecture guards for host / non-AVR compatibility
    if STDCPP_ROOT.exists():
        for root, dirs, files in os.walk(STDCPP_ROOT):
            rel_dir = Path(root).relative_to(STDCPP_ROOT)
            target_dir = src_dir / rel_dir
            target_dir.mkdir(parents=True, exist_ok=True)
            for f in files:
                src_file = Path(root) / f
                dst_file = target_dir / f
                content = src_file.read_text(encoding="utf-8", errors="replace")
                if content.startswith("#pragma GCC system_header // avr-libstdcpp"):
                    rel_header = (rel_dir / f).as_posix() if str(rel_dir) != "." else f
                    wrapped_content = (
                        f"#pragma GCC system_header // avr-libstdcpp\n"
                        f"#if !defined(__AVR__) && !defined(ARDUINO_ARCH_AVR)\n"
                        f"#if __has_include_next(<{rel_header}>)\n"
                        f"#include_next <{rel_header}>\n"
                        f"#endif\n"
                        f"#else\n"
                        f"{content}\n"
                        f"#endif\n"
                    )
                    dst_file.write_text(wrapped_content, encoding="utf-8")
                else:
                    shutil.copy2(src_file, dst_file)

    # 4.2 Patch cstdio to provide std::snprintf and std::vsnprintf on AVR
    for cstdio_path in dest_dir.glob("**/cstdio"):
        content = cstdio_path.read_text(encoding="utf-8", errors="replace")
        if "using ::vsprintf;\n}" in content and "using ::vsnprintf;\n}" not in content:
            content = content.replace(
                "  using ::vsprintf;\n}",
                "  using ::vsprintf;\n  using ::snprintf;\n  using ::vsnprintf;\n}",
            )
            cstdio_path.write_text(content, encoding="utf-8")

    # 4.3 Patch std::has_unique_object_representations_v for GCC < 8 compatibility
    poly = """
#if !defined(__clang__) && defined(__GNUC__) && (__GNUC__ < 8)
#ifndef _PW_HAS_UNIQUE_OBJECT_REPRESENTATIONS_V_DEFINED
#define _PW_HAS_UNIQUE_OBJECT_REPRESENTATIONS_V_DEFINED
namespace std {
template <typename T>
constexpr bool has_unique_object_representations_v = is_trivially_copyable<T>::value;
} // namespace std
#endif
#endif
"""
    for hdr in [
        src_dir / "pw_bytes" / "span.h",
        src_dir / "lib" / "stdcompat" / "bit.h",
    ]:
        if hdr.exists():
            c = hdr.read_text(encoding="utf-8", errors="replace")
            if (
                "has_unique_object_representations_v" in c
                and "_PW_HAS_UNIQUE_OBJECT_REPRESENTATIONS_V_DEFINED" not in c
            ):
                c = c.replace(
                    "#include <type_traits>", "#include <type_traits>" + poly, 1
                )
                hdr.write_text(c, encoding="utf-8")

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
