#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    // Normalize minor formatting differences introduced by auto-formatters.
    while (!content.empty() && content.back() == '\n') {
        content.pop_back();
    }
    if (!content.empty() && content.back() == '\r') {
        content.pop_back();
    }

    std::ostringstream normalized;
    std::istringstream lines(content);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        for (std::string::size_type pos = 0; (pos = line.find("// ", pos)) != std::string::npos;) {
            line.erase(pos + 2, 1);
        }
        if (!first) {
            normalized << '\n';
        }
        first = false;
        normalized << line;
    }
    return normalized.str();
}

fs::path RepoRoot() {
    return fs::path(__FILE__).parent_path().parent_path();
}

struct TempCopy {
    fs::path path;
    TempCopy(const fs::path &source, const fs::path &dir, const std::string &suffix) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        path = dir / (source.stem().string() + suffix + source.extension().string());
        fs::copy_file(source, path, fs::copy_options::overwrite_existing, ec);
    }
    ~TempCopy() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

int RunTool(const fs::path &tool_path, const fs::path &build_dir, const fs::path &file_path) {
    std::string command = "\"" + tool_path.string() + "\" -p \"" + build_dir.string() + "\" \"" +
                          file_path.string() + "\" --";
    return std::system(command.c_str());
}

void RunAndCompare(const fs::path &input, const fs::path &expected, const std::string &suffix) {
    fs::path root = RepoRoot();
    fs::path tool_path = root / "build" / "refactor_tool";
    fs::path build_dir = root / "build";
    fs::path temp_dir = root / "tests" / "tests_data" / "tmp_gtest";

    ASSERT_TRUE(fs::exists(input)) << "Missing input: " << input.string();
    ASSERT_TRUE(fs::exists(expected)) << "Missing expected: " << expected.string();
    ASSERT_TRUE(fs::exists(tool_path)) << "Missing tool: " << tool_path.string();
    ASSERT_TRUE(fs::exists(build_dir / "compile_commands.json"))
        << "Missing compile database in: " << build_dir.string();

    TempCopy temp(input, temp_dir, suffix);
    ASSERT_TRUE(fs::exists(temp.path)) << "Temp file not created: " << temp.path.string();

    int result = RunTool(tool_path, build_dir, temp.path);
    ASSERT_EQ(result, 0) << "Refactor tool failed with code " << result;

    EXPECT_EQ(ReadFile(temp.path), ReadFile(expected));
}

}  // namespace

TEST(RefactorVirtualDestructor, InsertsVirtualOnNonVirtual) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test1.cpp",
                  root / "tests" / "tests_data" / "test1_ref.cpp", "_virtual_pos");
}

TEST(RefactorVirtualDestructor, NoChangeWhenAlreadyVirtual) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test1_ref.cpp",
                  root / "tests" / "tests_data" / "test1_ref.cpp", "_virtual_neg");
}

TEST(RefactorOverride, AddsOverrideToOverriddenMethods) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test2.cpp",
                  root / "tests" / "tests_data" / "test2_ref.cpp", "_override_pos");
}

TEST(RefactorOverride, NoChangeWhenOverridePresent) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test2_ref.cpp",
                  root / "tests" / "tests_data" / "test2_ref.cpp", "_override_neg");
}

TEST(RefactorRangeFor, AddsReferenceForConstNonBuiltin) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test3.cpp",
                  root / "tests" / "tests_data" / "test3_ref.cpp", "_range_pos");
}

TEST(RefactorRangeFor, NoChangeForAlreadyRefOrBuiltin) {
    fs::path root = RepoRoot();
    RunAndCompare(root / "tests" / "tests_data" / "test3_ref.cpp",
                  root / "tests" / "tests_data" / "test3_ref.cpp", "_range_neg");
}
