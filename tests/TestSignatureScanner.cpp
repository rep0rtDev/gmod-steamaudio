// tests/TestSignatureScanner.cpp
#include "TestFramework.h"
#include "util/SignatureScanner.h"

#include <cstring>
#include <vector>

using namespace sa;

SA_TEST(Pattern_ParsesIdaStyle)
{
    const auto p = Pattern::Parse("55 8B EC ?? ? 83 EC 08");
    SA_CHECK(p.has_value());
    SA_CHECK_EQ(p->Size(), size_t(8));
    SA_CHECK(p->mask[0] && !p->mask[3] && !p->mask[4] && p->mask[5]);
    SA_CHECK_EQ(int(p->bytes[1]), 0x8B);
    SA_CHECK(!Pattern::Parse("ZZ 00").has_value());
    SA_CHECK(!Pattern::Parse("").has_value() || Pattern::Parse("")->Empty());
}

SA_TEST(Pattern_ParsesCodeStyle)
{
    const auto p = Pattern::ParseCodeStyle(std::string("\x55\x8B\xEC\x00", 4), "xxx?");
    SA_CHECK(p.has_value());
    SA_CHECK_EQ(p->Size(), size_t(4));
    SA_CHECK(!p->mask[3]);
}

SA_TEST(IsMemoryReadable_RejectsWrappingRanges)
{
    SA_CHECK(!IsMemoryReadable(0, 8));
    SA_CHECK(!IsMemoryReadable(0x1000, 0));
    SA_CHECK(!IsMemoryReadable(~uintptr_t(0), sizeof(void*)));
    SA_CHECK(!IsMemoryReadable(~uintptr_t(0) - 3, sizeof(void*)));
    const std::vector<uint8_t> local(64, 0);
    SA_CHECK(IsMemoryReadable(reinterpret_cast<uintptr_t>(local.data()), local.size()));
}

SA_TEST(VTableSlots_ValidatesEverySlotInOneLayout)
{
    const uint8_t code[16] = {};
    const auto image = MakeBufferImage(code, sizeof(code));
    const uintptr_t base = reinterpret_cast<uintptr_t>(code);
    uintptr_t table[] = {base, base + 1, base + 2, base + 3};
    const uintptr_t* object = table;
    SA_CHECK(HasCodeVTableSlots(&object, image, {0, 1, 2, 3}));
    SA_CHECK(HasCodeVTableSlots(&object, image, {3, 0, 3}));
    table[2] = 0;
    SA_CHECK(!HasCodeVTableSlots(&object, image, {0, 1, 2, 3}));
    SA_CHECK(HasCodeVTableSlots(&object, image, {0, 1, 3}));
    table[2] = base + sizeof(code);
    SA_CHECK(!HasCodeVTableSlots(&object, image, {2}));
    SA_CHECK(!HasCodeVTableSlots(&object, image, {-1, 0}));
    SA_CHECK(!HasCodeVTableSlots(&object, image, {0, 256}));
    SA_CHECK(!HasCodeVTableSlots(&object, image, {}));
    SA_CHECK(!HasCodeVTableSlots(nullptr, image, {0}));
    SA_CHECK(!HasCodeVTableSlots(&object, MakeBufferImage(code, sizeof(code), ".data", false), {0}));
    object = nullptr;
    SA_CHECK(!HasCodeVTableSlots(&object, image, {0}));
}

SA_TEST(Scanner_FindsPatternsInBuffer)
{
    std::vector<uint8_t> code(4096, 0x90);
    const uint8_t needle[] = {0x55, 0x8B, 0xEC, 0x12, 0x34, 0x83, 0xEC, 0x08};
    std::memcpy(code.data() + 100, needle, sizeof(needle));
    std::memcpy(code.data() + 3000, needle, sizeof(needle));
    code[3003] = 0x99; // differs only in the wildcard position

    SignatureScanner scanner(MakeBufferImage(code.data(), code.size()));
    const auto p = Pattern::Parse("55 8B EC ? 34 83 EC 08");
    SA_CHECK(p.has_value());
    const auto all = scanner.FindAll(*p);
    SA_CHECK_EQ(all.size(), size_t(2));
    SA_CHECK(all[0] == reinterpret_cast<uintptr_t>(code.data()) + 100);
    SA_CHECK(!scanner.FindUnique(*p).has_value());
    const auto exact = Pattern::Parse("55 8B EC 12 34 83 EC 08");
    SA_CHECK(scanner.FindUnique(*exact).has_value());
}

SA_TEST(Scanner_FindsStringsAndPointers)
{
    std::vector<uint8_t> data(1024, 0);
    const char literal[] = "IAudioDevice";
    std::memcpy(data.data() + 64, literal, sizeof(literal));
    const uintptr_t target = 0x1122334455667788ull & (sizeof(void*) == 8 ? ~0ull : 0xFFFFFFFFull);
    std::memcpy(data.data() + 512, &target, sizeof(target));

    SignatureScanner scanner(MakeBufferImage(data.data(), data.size(), ".rdata", false));
    const auto strings = scanner.FindString("IAudioDevice");
    SA_CHECK_EQ(strings.size(), size_t(1));
    SA_CHECK(strings[0] == reinterpret_cast<uintptr_t>(data.data()) + 64);
    SA_CHECK(scanner.FindString("IAudio", true).empty());
    SA_CHECK_EQ(scanner.FindString("IAudio", false).size(), size_t(1));
    const auto ptrs = scanner.FindPointers(target);
    SA_CHECK_EQ(ptrs.size(), size_t(1));
}

SA_TEST(Scanner_ResolvesRelativeDisplacement)
{
    // e8 <rel32> : call target = next instruction + rel32
    std::vector<uint8_t> code(64, 0x90);
    const int32_t rel = 0x20;
    code[10] = 0xE8;
    std::memcpy(code.data() + 11, &rel, 4);
    const uintptr_t instr = reinterpret_cast<uintptr_t>(code.data()) + 10;
    const auto target = SignatureScanner::ResolveRelative(instr, 1, 5);
    SA_CHECK(target.has_value());
    SA_CHECK(*target == instr + 5 + 0x20);
}

SA_TEST(Scanner_LeadingWildcardsRespectSectionEnd)
{
    const uint8_t bytes[] = {0, 0, 0, 0xAA};
    SignatureScanner full(MakeBufferImage(bytes, sizeof(bytes)));
    SA_CHECK_EQ(full.FindAll(*Pattern::Parse("? AA")).size(), size_t(1));
    SA_CHECK_EQ(full.FindAll(*Pattern::Parse("? ? AA")).size(), size_t(1));
    SignatureScanner shortSection(MakeBufferImage(bytes, 3));
    SA_CHECK(shortSection.FindAll(*Pattern::Parse("? ? AA")).empty());
}

SA_TEST(ParseInteger_DecimalAndHex)
{
    SA_CHECK(ParseInteger("42").value_or(-1) == 42);
    SA_CHECK(ParseInteger("0x1F").value_or(-1) == 31);
    SA_CHECK(ParseInteger("-8").value_or(0) == -8);
    SA_CHECK(!ParseInteger("abc").has_value());
}
