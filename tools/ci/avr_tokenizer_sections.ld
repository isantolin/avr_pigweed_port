/*
 * Pigweed tokenizer metadata and dictionary are host-side diagnostic data.
 * Keep them in non-allocated INFO sections instead of consuming AVR SRAM.
 * INSERT augments the AVR default linker script; passing Pigweed's raw
 * SECTIONS fragment with -T would replace the default memory layout.
 */
SECTIONS
{
  .pw_tokenizer.info 0 (INFO) :
  {
    KEEP(*(.pw_tokenizer.info))
  }

  .pw_tokenizer.entries 0 (INFO) :
  {
    KEEP(*(.pw_tokenizer.entries.*))
    KEEP(*(.rodata.*_pw_tokenizer_string_entry_*))
  }
}
INSERT AFTER .data;
