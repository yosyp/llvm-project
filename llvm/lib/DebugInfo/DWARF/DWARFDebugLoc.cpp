//===- DWARFDebugLoc.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/DWARFDebugLoc.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFExpression.h"
#include "llvm/DebugInfo/DWARF/DWARFRelocMap.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cinttypes>
#include <cstdint>

using namespace llvm;

// When directly dumping the .debug_loc without a compile unit, we have to guess
// at the DWARF version. This only affects DW_OP_call_ref, which is a rare
// expression that LLVM doesn't produce. Guessing the wrong version means we
// won't be able to pretty print expressions in DWARF2 binaries produced by
// non-LLVM tools.
static void dumpExpression(raw_ostream &OS, ArrayRef<uint8_t> Data,
                           bool IsLittleEndian, unsigned AddressSize,
                           const MCRegisterInfo *MRI, DWARFUnit *U) {
  DWARFDataExtractor Extractor(toStringRef(Data), IsLittleEndian, AddressSize);
  DWARFExpression(Extractor, dwarf::DWARF_VERSION, AddressSize).print(OS, MRI, U);
}

void DWARFDebugLoc::LocationList::dump(raw_ostream &OS, uint64_t BaseAddress,
                                       bool IsLittleEndian,
                                       unsigned AddressSize,
                                       const MCRegisterInfo *MRI, DWARFUnit *U,
                                       unsigned Indent) const {
  for (const Entry &E : Entries) {
    OS << '\n';
    OS.indent(Indent);
    OS << format("[0x%*.*" PRIx64 ", ", AddressSize * 2, AddressSize * 2,
                 BaseAddress + E.Begin);
    OS << format(" 0x%*.*" PRIx64 ")", AddressSize * 2, AddressSize * 2,
                 BaseAddress + E.End);
    OS << ": ";

    dumpExpression(OS, E.Loc, IsLittleEndian, AddressSize, MRI, U);
  }
}

DWARFDebugLoc::LocationList const *
DWARFDebugLoc::getLocationListAtOffset(uint64_t Offset) const {
  auto It = partition_point(
      Locations, [=](const LocationList &L) { return L.Offset < Offset; });
  if (It != Locations.end() && It->Offset == Offset)
    return &(*It);
  return nullptr;
}

void DWARFDebugLoc::dump(raw_ostream &OS, const MCRegisterInfo *MRI,
                         Optional<uint64_t> Offset) const {
  auto DumpLocationList = [&](const LocationList &L) {
    OS << format("0x%8.8" PRIx64 ": ", L.Offset);
    L.dump(OS, 0, IsLittleEndian, AddressSize, MRI, nullptr, 12);
    OS << "\n\n";
  };

  if (Offset) {
    if (auto *L = getLocationListAtOffset(*Offset))
      DumpLocationList(*L);
    return;
  }

  for (const LocationList &L : Locations) {
    DumpLocationList(L);
  }
}

Expected<DWARFDebugLoc::LocationList>
DWARFDebugLoc::parseOneLocationList(const DWARFDataExtractor &Data,
                                    uint64_t *Offset) {
  LocationList LL;
  LL.Offset = *Offset;
  DataExtractor::Cursor C(*Offset);

  // 2.6.2 Location Lists
  // A location list entry consists of:
  while (true) {
    Entry E;

    // 1. A beginning address offset. ...
    E.Begin = Data.getRelocatedAddress(C);

    // 2. An ending address offset. ...
    E.End = Data.getRelocatedAddress(C);

    if (Error Err = C.takeError())
      return std::move(Err);

    // The end of any given location list is marked by an end of list entry,
    // which consists of a 0 for the beginning address offset and a 0 for the
    // ending address offset.
    if (E.Begin == 0 && E.End == 0) {
      *Offset = C.tell();
      return LL;
    }

    if (E.Begin != (AddressSize == 4 ? -1U : -1ULL)) {
      unsigned Bytes = Data.getU16(C);
      // A single location description describing the location of the object...
      Data.getU8(C, E.Loc, Bytes);
    }

    LL.Entries.push_back(std::move(E));
  }
}

void DWARFDebugLoc::parse(const DWARFDataExtractor &data) {
  IsLittleEndian = data.isLittleEndian();
  AddressSize = data.getAddressSize();

  uint64_t Offset = 0;
  while (Offset < data.getData().size()) {
    if (auto LL = parseOneLocationList(data, &Offset))
      Locations.push_back(std::move(*LL));
    else {
      logAllUnhandledErrors(LL.takeError(), WithColor::error());
      break;
    }
  }
}

Expected<DWARFDebugLoclists::LocationList>
DWARFDebugLoclists::parseOneLocationList(const DataExtractor &Data,
                                         uint64_t *Offset, unsigned Version) {
  LocationList LL;
  LL.Offset = *Offset;
  DataExtractor::Cursor C(*Offset);

  // dwarf::DW_LLE_end_of_list_entry is 0 and indicates the end of the list.
  while (auto Kind = static_cast<dwarf::LoclistEntries>(Data.getU8(C))) {
    Entry E;
    E.Kind = Kind;
    switch (Kind) {
    case dwarf::DW_LLE_startx_length:
      E.Value0 = Data.getULEB128(C);
      // Pre-DWARF 5 has different interpretation of the length field. We have
      // to support both pre- and standartized styles for the compatibility.
      if (Version < 5)
        E.Value1 = Data.getU32(C);
      else
        E.Value1 = Data.getULEB128(C);
      break;
    case dwarf::DW_LLE_start_length:
      E.Value0 = Data.getAddress(C);
      E.Value1 = Data.getULEB128(C);
      break;
    case dwarf::DW_LLE_offset_pair:
      E.Value0 = Data.getULEB128(C);
      E.Value1 = Data.getULEB128(C);
      break;
    case dwarf::DW_LLE_base_address:
      E.Value0 = Data.getAddress(C);
      break;
    default:
      cantFail(C.takeError());
      return createStringError(errc::illegal_byte_sequence,
                               "LLE of kind %x not supported", (int)Kind);
    }

    if (Kind != dwarf::DW_LLE_base_address) {
      unsigned Bytes = Version >= 5 ? Data.getULEB128(C) : Data.getU16(C);
      // A single location description describing the location of the object...
      Data.getU8(C, E.Loc, Bytes);
    }

    LL.Entries.push_back(std::move(E));
  }
  if (Error Err = C.takeError())
    return std::move(Err);
  *Offset = C.tell();
  return LL;
}

void DWARFDebugLoclists::parse(DataExtractor data, unsigned Version) {
  IsLittleEndian = data.isLittleEndian();
  AddressSize = data.getAddressSize();

  uint64_t Offset = 0;
  while (Offset < data.getData().size()) {
    if (auto LL = parseOneLocationList(data, &Offset, Version))
      Locations.push_back(std::move(*LL));
    else {
      logAllUnhandledErrors(LL.takeError(), WithColor::error());
      return;
    }
  }
}

DWARFDebugLoclists::LocationList const *
DWARFDebugLoclists::getLocationListAtOffset(uint64_t Offset) const {
  auto It = partition_point(
      Locations, [=](const LocationList &L) { return L.Offset < Offset; });
  if (It != Locations.end() && It->Offset == Offset)
    return &(*It);
  return nullptr;
}

void DWARFDebugLoclists::LocationList::dump(raw_ostream &OS, uint64_t BaseAddr,
                                            bool IsLittleEndian,
                                            unsigned AddressSize,
                                            const MCRegisterInfo *MRI,
                                            DWARFUnit *U,
                                            unsigned Indent) const {
  for (const Entry &E : Entries) {
    switch (E.Kind) {
    case dwarf::DW_LLE_startx_length:
      OS << '\n';
      OS.indent(Indent);
      OS << "Addr idx " << E.Value0 << " (w/ length " << E.Value1 << "): ";
      break;
    case dwarf::DW_LLE_start_length:
      OS << '\n';
      OS.indent(Indent);
      OS << format("[0x%*.*" PRIx64 ", 0x%*.*" PRIx64 "): ", AddressSize * 2,
                   AddressSize * 2, E.Value0, AddressSize * 2, AddressSize * 2,
                   E.Value0 + E.Value1);
      break;
    case dwarf::DW_LLE_offset_pair:
      OS << '\n';
      OS.indent(Indent);
      OS << format("[0x%*.*" PRIx64 ", 0x%*.*" PRIx64 "): ", AddressSize * 2,
                   AddressSize * 2, BaseAddr + E.Value0, AddressSize * 2,
                   AddressSize * 2, BaseAddr + E.Value1);
      break;
    case dwarf::DW_LLE_base_address:
      BaseAddr = E.Value0;
      break;
    default:
      llvm_unreachable("unreachable locations list kind");
    }

    dumpExpression(OS, E.Loc, IsLittleEndian, AddressSize, MRI, U);
  }
}

void DWARFDebugLoclists::dump(raw_ostream &OS, uint64_t BaseAddr,
                              const MCRegisterInfo *MRI,
                              Optional<uint64_t> Offset) const {
  auto DumpLocationList = [&](const LocationList &L) {
    OS << format("0x%8.8" PRIx64 ": ", L.Offset);
    L.dump(OS, BaseAddr, IsLittleEndian, AddressSize, MRI, nullptr, /*Indent=*/12);
    OS << "\n\n";
  };

  if (Offset) {
    if (auto *L = getLocationListAtOffset(*Offset))
      DumpLocationList(*L);
    return;
  }

  for (const LocationList &L : Locations) {
    DumpLocationList(L);
  }
}
