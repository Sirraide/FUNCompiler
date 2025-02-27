#ifndef GENERIC_OBJECT_H
#define GENERIC_OBJECT_H

#include <ast.h>
#include <stdint.h>
#include <string.h>
#include <vector.h>

typedef Vector(uint8_t) ByteBuffer;

typedef enum GObjSymbolType {
  GOBJ_SYMTYPE_NONE,
  GOBJ_SYMTYPE_FUNCTION,
  GOBJ_SYMTYPE_STATIC,
  GOBJ_SYMTYPE_EXPORT, // like static but global
  GOBJ_SYMTYPE_EXTERNAL,
  GOBJ_SYMTYPE_COUNT
} GObjSymbolType;

typedef struct GObjSymbol {
  GObjSymbolType type;
  // Name of symbol.
  char *name;
  // Name of section this symbol is associated with.
  char *section_name;
  // Offset within section where symbol is defined.
  size_t byte_offset;
} GObjSymbol;

typedef enum RelocationType {
  /// Relative to program counter
  RELOC_DISP32_PCREL,
  /// Absolute
  RELOC_DISP32,
} RelocationType;

typedef struct RelocationEntry {
  RelocationType type;
  GObjSymbol sym;
  /// Addend -> added to relocated value
  int64_t addend;
} RelocationEntry;
typedef Vector(RelocationEntry) Relocations;

typedef enum SectionAttributes {
  SEC_ATTR_WRITABLE = (1 << 0),
  SEC_ATTR_EXECUTABLE = (1 << 1),
  SEC_ATTR_SPAN_FILL = (1 << 31)
} SectionAttributes;

typedef struct Section {
  char *name;
  SectionAttributes attributes;
  union {
    ByteBuffer bytes;
    struct {
      uint8_t value;
      size_t amount;
    } fill;
  } data;
} Section;
typedef Vector(Section) Sections;

typedef Vector(GObjSymbol) Symbols;

typedef struct GenericObjectFile {
  // By convention, the code/text section is always present at the 0th index.
  Sections sections;
  Symbols symbols;
  Relocations relocs;
  // TODO: Debug info.
} GenericObjectFile;

/// Write 1 byte of data to section.
void sec_write_1(Section *section, uint8_t value);
/// Write 2 bytes of data to section.
void sec_write_2(Section *section, uint8_t value0, uint8_t value1);
/// Write 3 bytes of data to section.
void sec_write_3(Section *section, uint8_t value0, uint8_t value1, uint8_t value2);
/// Write 4 bytes of data to section.
void sec_write_4(Section *section, uint8_t value0, uint8_t value1, uint8_t value2, uint8_t value3);
/// Write n bytes of data from buffer to section.
void sec_write_n(Section *section, const void* buffer, size_t n);

/// Get the code/text section (always at the 0th index of sections vector)
Section *code_section(GenericObjectFile *object);

/// Write 1 byte of data to code.
void mcode_1(GenericObjectFile *object, uint8_t value);
/// Write 2 bytes of data to code.
void mcode_2(GenericObjectFile *object, uint8_t value0, uint8_t value1);
/// Write 3 bytes of data to code.
void mcode_3(GenericObjectFile *object, uint8_t value0, uint8_t value1, uint8_t value2);
/// Write 4 bytes of data to code.
void mcode_4(GenericObjectFile *object, uint8_t value0, uint8_t value1, uint8_t value2, uint8_t value3);
/// Write n bytes of data from buffer to code.
void mcode_n(GenericObjectFile *object, void* buffer, size_t n);

Section *get_section_by_name(const Sections sections, const char *name);

/// Write the given generic object file in ELF object file format into
/// a given file.
void generic_object_as_elf_x86_64(GenericObjectFile*, FILE*);
void generic_object_as_elf_x86_64_at_path(GenericObjectFile*, const char *path);

/// Write the given generic object file in COFF object file format into
/// a given file.
void generic_object_as_coff_x86_64(GenericObjectFile*, FILE*);
void generic_object_as_coff_x86_64_at_path(GenericObjectFile *object, const char *path);

/// Free any resources that have been allocated for the given generic
/// object file. Invalidates the passed-in generic object file, so don't
/// try to use it after calling this or it's UB.
void generic_object_delete(GenericObjectFile *object);

void generic_object_print(GenericObjectFile *object);

#endif /* GENERIC_OBJECT_H */
