#!/usr/bin/env python3
# File: tools/export_arduino_library.py
"""Packages Pigweed AVR port into a standalone Arduino IDE / Arduino CLI library."""

import os
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PW_ROOT = REPO_ROOT / "third_party" / "pigweed"
STDCPP_ROOT = REPO_ROOT / "third_party" / "avr-libstdcpp" / "include"


def export_library(dest_dir: Path):
    print(f"[*] Exporting Pigweed AVR Arduino Library to: {dest_dir}")
    if dest_dir.exists():
        shutil.rmtree(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)

    src_dir = dest_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)

    # 1. Copy library.properties
    shutil.copy2(REPO_ROOT / "library.properties", dest_dir / "library.properties")

    # 2. Copy root header and hardware adapters
    for f in ["PigweedAVR.h", "arduino_stream.h", "avr_uart_stream.h", "avr_stdio_compat.h", "cxx_abi_stubs.cpp"]:
        shutil.copy2(REPO_ROOT / "src" / f, src_dir / f)

    # 3. Copy custom AVR backends
    backends = ["pw_assert_avr", "pw_chrono_avr", "pw_crypto_avr", "pw_log_avr", "pw_rpc_avr"]
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
        "pw_cobs",
        "pw_function",
        "pw_string",
        "pw_base64",
        "pw_tokenizer",
        "pw_chrono",
        "pw_preprocessor",
        "pw_numeric",
        "pw_enum",
        "pw_memory",
        "pw_polyfill",
        "pw_assert",
        "pw_crypto",
        "pw_ring_buffer",
        "pw_log",
        "pw_toolchain",
    ]

    for mod in modules:
        mod_path = PW_ROOT / mod
        pub_dirs = [mod_path / "public", mod_path / "cpp" / "public"]
        for p in pub_dirs:
            if p.exists():
                shutil.copytree(p, src_dir, dirs_exist_ok=True)

        # Copy .cc sources for compilation
        for cc_file in mod_path.glob("*.cc"):
            if not cc_file.name.endswith(("_test.cc", "_fuzzer.cc", "_perf_test.cc")):
                shutil.copy2(cc_file, src_dir / f"{mod}_{cc_file.name}")
        for cc_file in (mod_path / "cpp").glob("*.cc"):
            if not cc_file.name.endswith(("_test.cc", "_fuzzer.cc")):
                shutil.copy2(cc_file, src_dir / f"{mod}_{cc_file.name}")

    # Fuchsia stdcompat and fit dependencies (required by pw_function)
    for fuchsia_lib in ["stdcompat", "fit"]:
        fuchsia_include = PW_ROOT / "third_party" / "fuchsia" / "repo" / "sdk" / "lib" / fuchsia_lib / "include"
        if fuchsia_include.exists():
            shutil.copytree(fuchsia_include, src_dir, dirs_exist_ok=True)

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

    # 5. Create an example sketch for Arduino Yún
    examples_dir = dest_dir / "examples" / "YunBridgeRpc"
    examples_dir.mkdir(parents=True, exist_ok=True)

    example_ino = """// Example Arduino Yún Pigweed RPC Server
#include <Arduino.h>
#include <PigweedAVR.h>

// Service to handle Linux RPC requests over Serial1 (USART1 connected to AR9331)
class YunBridgeService final : public pw::rpc_avr::RpcService {
 public:
  static constexpr uint32_t kId = 0x00010001;
  YunBridgeService() : RpcService(kId) {}

  pw::Status HandleMethod(uint32_t method_id,
                          pw::ConstByteSpan request,
                          pw::ByteSpan response_buffer,
                          size_t& response_size) override {
    if (method_id == 1) { // DigitalWrite(pin, val)
      if (request.size() < 2) return pw::Status::InvalidArgument();
      pinMode(static_cast<uint8_t>(request[0]), OUTPUT);
      digitalWrite(static_cast<uint8_t>(request[0]), static_cast<uint8_t>(request[1]));
      response_size = 0;
      return pw::OkStatus();
    }
    if (method_id == 2) { // DigitalRead(pin) -> val
      if (request.size() < 1) return pw::Status::InvalidArgument();
      if (response_buffer.empty()) return pw::Status::ResourceExhausted();
      response_buffer[0] = static_cast<std::byte>(digitalRead(static_cast<uint8_t>(request[0])));
      response_size = 1;
      return pw::OkStatus();
    }
    return pw::Status::NotFound();
  }
};

YunBridgeService bridge_service;
pw::rpc_avr::RpcServer<2> rpc_server;
avr_port::ArduinoStream yun_stream(Serial1);

void setup() {
  Serial1.begin(115200);
  rpc_server.RegisterService(bridge_service);
}

void loop() {
  std::byte rx_buf[64];
  auto read_res = yun_stream.Read(rx_buf);
  if (read_res.ok() && read_res.value().size() > 0) {
    std::byte resp_buf[64];
    auto rpc_res = rpc_server.ProcessPacket(read_res.value(), resp_buf);
    if (rpc_res.ok() && rpc_res.size() > 0) {
      yun_stream.Write(pw::span(resp_buf, rpc_res.size())).IgnoreError();
    }
  }
}
"""
    (examples_dir / "YunBridgeRpc.ino").write_text(example_ino)

    print(f"[+] Successfully exported Arduino Library ({len(list(src_dir.glob('*')))} components).")


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else (REPO_ROOT / "dist" / "PigweedAVR")
    export_library(out)
