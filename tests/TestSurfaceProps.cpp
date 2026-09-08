// tests/TestSurfaceProps.cpp
#include <string>
#include <vector>

#include "TestFramework.h"
#include "steamaudio/SceneBuilder.h"
#include "steamaudio/SurfaceProps.h"
#include "util/KeyValues.h"

using namespace sa;

namespace {

KvDocument Parse(const std::string& text)
{
    KvDocument doc;
    ParseKeyValues(text, doc);
    return doc;
}

const char* kSurfaceProps = R"(
// Copied shape of scripts/surfaceproperties.txt
"default"
{
	"density"	"2000"
	"elasticity"	"0.25"
	"gamematerial"	"C"
}

"metal"
{
	"density"	"2700"
	"gamematerial"	"M"
}

"metal_barrel"
{
	"base"	"metal"
	"impacthard"	"MetalBarrel.ImpactHard"
}

"Metal_Box"
{
	"base"	"metal_barrel"
	"thickness"	"0.5"
}

"Wood_Furniture"
{
	"base"	"wood_solid"
	"gamematerial"	"W"
}

"wood_solid"
{
	"base"	"wood"
}

"wood"
{
	"density"	"700"
	"gamematerial"	"W"
}

"plasticcup"
{
	"base"	"default"
	"gamematerial"	"L"
}

"loop_a"
{
	"base"	"loop_b"
}

"loop_b"
{
	"base"	"loop_a"
}
)";

} // namespace

// ---------------------------------------------------------------------------
// KeyValues
// ---------------------------------------------------------------------------

SA_TEST(KeyValues_ParsesQuotedUnquotedNestedAndComments)
{
    const std::string text = "\xEF\xBB\xBF"
                             "// header comment\n"
                             "\"LightmappedGeneric\" // trailing comment\n"
                             "{\n"
                             "  $basetexture concrete/concretefloor001a\n"
                             "  \"$surfaceprop\" \"Concrete\"\n"
                             "  Proxies { Sine { resultVar $alpha } }\n"
                             "  \"%keywords\" \"a, b\" [$WIN32]\n"
                             "  $win \"1\" [!$X360]\n"
                             "}\n";
    KvDocument doc;
    SA_CHECK(ParseKeyValues(text, doc));
    SA_CHECK(!doc.truncated);
    SA_CHECK_EQ(doc.roots.size(), size_t(1));
    const KvNode& root = doc.roots[0];
    SA_CHECK(root.isBlock);
    SA_CHECK(KvKeyEquals(root.key, "lightmappedgeneric"));
    SA_CHECK_STREQ(root.Get("$basetexture", ""), std::string("concrete/concretefloor001a"));
    SA_CHECK_STREQ(root.Get("$SURFACEPROP", ""), std::string("Concrete"));
    const KvNode* proxies = root.Find("proxies");
    SA_CHECK(proxies && proxies->isBlock);
    const KvNode* sine = proxies->Find("sine");
    SA_CHECK(sine && sine->isBlock);
    SA_CHECK_STREQ(sine->Get("resultvar", ""), std::string("$alpha"));
    const KvNode* keywords = root.Find("%keywords");
    SA_CHECK(keywords != nullptr);
    SA_CHECK_STREQ(keywords->condition, std::string("$WIN32"));
    SA_CHECK_STREQ(root.Find("$win")->condition, std::string("!$X360"));
    SA_CHECK_STREQ(root.Get("missing", "fallback"), std::string("fallback"));
}

SA_TEST(KeyValues_EscapesIncludesAndMultipleRoots)
{
    const std::string text = "#base \"base.vmt\"\n"
                             "#include shared/common.txt\n"
                             "a { k \"quoted \\\"inner\\\" \\\\ done\" }\n"
                             "b { }\n"
                             "c d\n";
    KvDocument doc;
    SA_CHECK(ParseKeyValues(text, doc));
    SA_CHECK_EQ(doc.includes.size(), size_t(2));
    SA_CHECK_STREQ(doc.includes[0], std::string("base.vmt"));
    SA_CHECK_STREQ(doc.includes[1], std::string("shared/common.txt"));
    SA_CHECK_EQ(doc.roots.size(), size_t(3));
    SA_CHECK_STREQ(doc.roots[0].Get("k", ""), std::string("quoted \"inner\" \\ done"));
    SA_CHECK(doc.roots[1].isBlock);
    SA_CHECK(doc.roots[1].children.empty());
    SA_CHECK(!doc.roots[2].isBlock);
    SA_CHECK_STREQ(doc.roots[2].value, std::string("d"));
}

SA_TEST(KeyValues_MalformedInputDoesNotCrash)
{
    KvDocument doc;
    SA_CHECK(!ParseKeyValues(std::string(), doc));
    SA_CHECK(doc.roots.empty());
    SA_CHECK(!doc.error.empty());
    SA_CHECK(!ParseKeyValues("   // only a comment\n", doc));

    SA_CHECK(!ParseKeyValues("\"unterminated { $surfaceprop metal", doc));
    SA_CHECK(doc.truncated);
    SA_CHECK(ParseKeyValues("a { b { c d ", doc));
    SA_CHECK(doc.truncated);
    SA_CHECK_EQ(doc.roots.size(), size_t(1));
    SA_CHECK_STREQ(doc.roots[0].Find("b")->Get("c", ""), std::string("d"));

    SA_CHECK(ParseKeyValues("} } } a b { }", doc)); // stray braces and a keyless block are dropped
    SA_CHECK(doc.truncated);
    SA_CHECK_EQ(doc.roots.size(), size_t(1));
    SA_CHECK_STREQ(doc.roots[0].key, std::string("a"));
    SA_CHECK_STREQ(doc.roots[0].value, std::string("b"));

    SA_CHECK(ParseKeyValues("a { b }", doc));
    SA_CHECK_EQ(doc.roots[0].children.size(), size_t(1));
    SA_CHECK_STREQ(doc.roots[0].children[0].key, std::string("b"));

    std::string embeddedNul = "a { b \"c\" ";
    embeddedNul.push_back('\0');
    embeddedNul += " d e }";
    SA_CHECK(ParseKeyValues(embeddedNul, doc));
    SA_CHECK_STREQ(doc.roots[0].Get("d", ""), std::string("e"));

    std::string deep;
    for (int i = 0; i < 200; ++i)
        deep += "n {";
    deep += " leaf v ";
    for (int i = 0; i < 200; ++i)
        deep += "}";
    KvLimits limits;
    limits.maxDepth = 8;
    SA_CHECK(ParseKeyValues(deep.data(), deep.size(), doc, limits));
    SA_CHECK(doc.truncated);
    SA_CHECK_EQ(doc.roots.size(), size_t(1));

    std::string many;
    for (int i = 0; i < 100; ++i)
        many += "k v\n";
    limits = KvLimits{};
    limits.maxNodes = 10;
    SA_CHECK(ParseKeyValues(many.data(), many.size(), doc, limits));
    SA_CHECK(doc.truncated);
    SA_CHECK_EQ(doc.nodes, size_t(10));
    SA_CHECK_EQ(doc.roots.size(), size_t(10));

    limits = KvLimits{};
    limits.maxTokenLength = 8;
    const std::string longToken = "k \"" + std::string(64, 'x') + "\" next 1";
    SA_CHECK(ParseKeyValues(longToken.data(), longToken.size(), doc, limits));
    SA_CHECK(doc.truncated);
}

// ---------------------------------------------------------------------------
// surfaceproperties.txt
// ---------------------------------------------------------------------------

SA_TEST(SurfaceProps_ParsesScriptAndInherits)
{
    SurfacePropDatabase db;
    std::string error;
    SA_CHECK_EQ(db.AddScript(kSurfaceProps, &error), size_t(10));
    SA_CHECK_EQ(db.Size(), size_t(10));

    const SurfacePropEntry* box = db.Find("METAL_BOX");
    SA_CHECK(box != nullptr);
    SA_CHECK_STREQ(box->name, std::string("metal_box"));
    SA_CHECK_STREQ(box->base, std::string("metal_barrel"));
    SA_CHECK(std::fabs(box->thickness - 0.5f) < 1e-6f);
    SA_CHECK_EQ(box->gameMaterial, char(0));
    SA_CHECK(std::fabs(db.Find("metal")->density - 2700.f) < 1e-3f);
    SA_CHECK_EQ(db.Find("metal")->gameMaterial, 'M');
    SA_CHECK(db.Find("nothing") == nullptr);

    SA_CHECK_STREQ(std::string(SurfacePropDatabase::GameMaterialName('C')), std::string("concrete"));
    SA_CHECK_STREQ(std::string(SurfacePropDatabase::GameMaterialName('m')), std::string("metal"));
    SA_CHECK_STREQ(std::string(SurfacePropDatabase::GameMaterialName('Y')), std::string("glass"));
    SA_CHECK(SurfacePropDatabase::GameMaterialName('?') == nullptr);

    // Known-set: only families the material library implements.
    const SurfacePropDatabase::KnownFn known = [](const std::string& n) {
        return n == "metal" || n == "wood" || n == "concrete" || n == "plastic";
    };
    SA_CHECK_STREQ(db.Canonicalize("metal_box", known), std::string("metal"));          // base chain
    SA_CHECK_STREQ(db.Canonicalize("Wood_Furniture", known), std::string("wood"));      // 2-level chain
    SA_CHECK_STREQ(db.Canonicalize("plasticcup", known), std::string("plastic"));       // gamematerial
    SA_CHECK_STREQ(db.Canonicalize("default", known), std::string("concrete"));         // gamematerial only
    SA_CHECK_STREQ(db.Canonicalize("loop_a", known), std::string());                    // cycle, no info
    SA_CHECK_STREQ(db.Canonicalize("unknown_prop", known), std::string());
    SA_CHECK_STREQ(db.Canonicalize("metal", known), std::string("metal"));

    // Second script overrides / extends.
    SA_CHECK_EQ(db.AddScript("\"metal\" { gamematerial W } \"extra\" { base metal }", &error), size_t(2));
    SA_CHECK_EQ(db.Size(), size_t(11));
    SA_CHECK_EQ(db.Find("metal")->gameMaterial, 'W');

    SA_CHECK_EQ(db.AddScript("", &error), size_t(0));
    SA_CHECK(!error.empty());
    SA_CHECK_EQ(db.AddScript("just a leaf", &error), size_t(0));
}

SA_TEST(SurfaceProps_ParsesManifest)
{
    const std::string manifest = "surfaceproperties_manifest\n"
                                 "{\n"
                                 "\t\"file\"\t\"scripts/surfaceproperties.txt\"\n"
                                 "\t\"file\"\t\"scripts\\\\surfaceproperties_hl2.txt\"\n"
                                 "\t\"file\"\t\"scripts/surfaceproperties_manifest.txt\" // self reference\n"
                                 "\t\"file\"\t\"scripts/surfaceproperties.txt\"\n"
                                 "\t\"other\"\t\"ignored\"\n"
                                 "}\n";
    const std::vector<std::string> files = SurfacePropDatabase::ParseManifest(manifest.data(), manifest.size());
    SA_CHECK_EQ(files.size(), size_t(2));
    SA_CHECK_STREQ(files[0], std::string("scripts/surfaceproperties.txt"));
    SA_CHECK_STREQ(files[1], std::string("scripts/surfaceproperties_hl2.txt"));

    SA_CHECK(SurfacePropDatabase::ParseManifest("", 0).empty());
    SA_CHECK(SurfacePropDatabase::ParseManifest("garbage { {", 11).empty());
}

// ---------------------------------------------------------------------------
// VMT
// ---------------------------------------------------------------------------

SA_TEST(Vmt_DirectSurfaceProp)
{
    std::string include;
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("LightmappedGeneric { $basetexture x \"$SurfaceProp\" \"Metal\" }"), &include),
                std::string("metal"));
    SA_CHECK(include.empty());

    // $surfaceprop inside a nested fallback block (e.g. LightmappedGeneric_DX8).
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("VertexLitGeneric { $basetexture x \"<dx90\" { $surfaceprop wood } }"), &include),
                std::string("wood"));

    SA_CHECK_STREQ(VmtSurfaceProp(Parse("UnlitGeneric { $basetexture x }"), &include), std::string());
    SA_CHECK_STREQ(VmtSurfaceProp(Parse(""), &include), std::string());
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("Not a block"), &include), std::string());
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("\"LightmappedGeneric\" { $surfaceprop \"\" }"), &include), std::string());
}

SA_TEST(Vmt_PatchInsertReplaceAndInclude)
{
    std::string include;
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("Patch { include \"materials/a/b.vmt\" insert { $surfaceprop Wood } }"), &include),
                std::string("wood"));
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("patch { include materials/a/b.vmt replace { \"$surfaceprop\" \"Glass\" } }"),
                               &include),
                std::string("glass"));
    // Replace wins over insert (mirrors what the engine ends up with).
    SA_CHECK_STREQ(VmtSurfaceProp(Parse("Patch { insert { $surfaceprop wood } replace { $surfaceprop metal } }"), &include),
                std::string("metal"));

    SA_CHECK_STREQ(VmtSurfaceProp(Parse("Patch { include \"Materials\\\\Concrete\\\\Wall.VMT\" insert { $detail x } }"),
                               &include),
                std::string());
    SA_CHECK_STREQ(include, std::string("materials/concrete/wall.vmt"));

    SA_CHECK_STREQ(VmtSurfaceProp(Parse("Patch { insert { $detail x } }"), &include), std::string());
    SA_CHECK(include.empty());
}

SA_TEST(Vmt_TextureToVmtPath)
{
    SA_CHECK_STREQ(TextureToVmtPath("concrete/concretefloor001a"), std::string("materials/concrete/concretefloor001a.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("CONCRETE\\ConcreteFloor001A"), std::string("materials/concrete/concretefloor001a.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("materials/metal/metalwall.vmt"), std::string("materials/metal/metalwall.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("/metal/metalwall.vtf"), std::string("materials/metal/metalwall.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("maps/gm_construct/concrete/floor_1_2_3"),
                std::string("materials/maps/gm_construct/concrete/floor_1_2_3.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("tools/toolsnodraw"), std::string("materials/tools/toolsnodraw.vmt"));
    SA_CHECK_STREQ(TextureToVmtPath("../../boot.ini"), std::string());
    SA_CHECK_STREQ(TextureToVmtPath(""), std::string());
    SA_CHECK_STREQ(TextureToVmtPath("   "), std::string());
}

// ---------------------------------------------------------------------------
// MaterialLibrary integration
// ---------------------------------------------------------------------------

SA_TEST(MaterialLibrary_UsesSurfacePropDatabase)
{
    MaterialLibrary lib;
    // Without the database only exact names/aliases/heuristics work.
    SA_CHECK_STREQ(lib.ResolveName("metal"), std::string("metal"));
    SA_CHECK_STREQ(lib.ResolveName("Metal_Box"), std::string("metal_thin"));  // built-in alias
    SA_CHECK_STREQ(lib.ResolveName("papercup"), std::string("paper"));        // name heuristic ("paper" substring)
    SA_CHECK_STREQ(lib.ResolveName("strange_thing"), std::string("generic"));
    SA_CHECK_STREQ(lib.ResolveName("zzz_custom_prop"), std::string("generic"));
    SA_CHECK_STREQ(lib.ResolveName(""), std::string("generic"));

    auto db = std::make_shared<SurfacePropDatabase>();
    const std::string script = std::string(kSurfaceProps) +
                               "\"zzz_custom_prop\" { base wood_solid }\n"
                               "\"strange_thing\" { gamematerial Y }\n"
                               "\"no_info\" { density 1 }\n";
    SA_CHECK(db->AddScript(script) == 13);
    lib.SetSurfaceProps(db);
    SA_CHECK(lib.SurfaceProps() == db.get());

    SA_CHECK_STREQ(lib.ResolveName("zzz_custom_prop"), std::string("wood"));  // base chain -> wood
    SA_CHECK_STREQ(lib.ResolveName("strange_thing"), std::string("glass"));   // gamematerial Y
    SA_CHECK_STREQ(lib.ResolveName("no_info"), std::string("generic"));
    SA_CHECK_STREQ(lib.ResolveName("Metal_Box"), std::string("metal_thin")); // alias still wins over the chain
    SA_CHECK_STREQ(lib.ResolveName("wood_furniture"), std::string("wood"));

    // FromSurfaceProp returns the same material the name resolves to.
    const IPLMaterial& wood = lib.FromSurfaceProp("wood");
    const IPLMaterial& viaChain = lib.FromSurfaceProp("zzz_custom_prop");
    SA_CHECK(&wood == &viaChain);
    SA_CHECK(&lib.FromSurfaceProp("no_info") == &lib.FromSurfaceProp("generic"));
    SA_CHECK(&lib.FromSurfaceProp("totally_unknown") == &lib.FromSurfaceProp("generic"));
    SA_CHECK(&lib.FromTextureName("concrete/concretefloor001a") == &lib.FromSurfaceProp("concrete"));
    SA_CHECK_STREQ(lib.NameFromTexture("tools/toolsnodraw"), std::string("generic"));

    lib.SetSurfaceProps(nullptr);
    SA_CHECK_STREQ(lib.ResolveName("zzz_custom_prop"), std::string("generic"));
}
