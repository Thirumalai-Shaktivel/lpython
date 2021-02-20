#include <chrono>
#include <iostream>
#include <stdlib.h>

#include <bin/CLI11.hpp>
#include <bin/tpl/whereami/whereami.h>

#include <lfortran/stacktrace.h>
#include <lfortran/parser/parser.h>
#include <lfortran/pickle.h>
#include <lfortran/semantics/ast_to_asr.h>
#include <lfortran/mod_to_asr.h>
#include <lfortran/codegen/asr_to_llvm.h>
#include <lfortran/codegen/asr_to_cpp.h>
#include <lfortran/codegen/asr_to_x86.h>
#include <lfortran/ast_to_src.h>
#include <lfortran/codegen/evaluator.h>
#include <lfortran/pass/do_loops.h>
#include <lfortran/pass/global_stmts.h>
#include <lfortran/asr_utils.h>
#include <lfortran/config.h>
#include <lfortran/fortran_kernel.h>
#include <lfortran/string_utils.h>

#include <cpp-terminal/terminal.h>
#include <cpp-terminal/prompt0.h>

namespace {

using LFortran::endswith;

enum Backend {
    llvm, cpp, x86
};

enum ASRPass {
    do_loops, global_stmts
};

std::string remove_extension(const std::string& filename) {
    size_t lastdot = filename.find_last_of(".");
    if (lastdot == std::string::npos) return filename;
    return filename.substr(0, lastdot);
}

std::string remove_path(const std::string& filename) {
    size_t lastslash = filename.find_last_of("/");
    if (lastslash == std::string::npos) return filename;
    return filename.substr(lastslash+1);
}

std::string read_file(const std::string &filename)
{
    std::ifstream ifs(filename.c_str(), std::ios::in | std::ios::binary
            | std::ios::ate);

    std::ifstream::pos_type filesize = ifs.tellg();
    if (filesize < 0) return std::string();

    ifs.seekg(0, std::ios::beg);

    std::vector<char> bytes(filesize);
    ifs.read(&bytes[0], filesize);

    return std::string(&bytes[0], filesize);
}

std::string get_kokkos_dir()
{
    char *env_p = std::getenv("LFORTRAN_KOKKOS_DIR");
    if (env_p) return env_p;
    std::cerr << "The code C++ generated by the C++ LFortran backend uses the Kokkos library" << std::endl;
    std::cerr << "(https://github.com/kokkos/kokkos). Please define the LFORTRAN_KOKKOS_DIR" << std::endl;
    std::cerr << "environment variable to point to the Kokkos installation." << std::endl;
    throw LFortran::LFortranException("LFORTRAN_KOKKOS_DIR is not defined");
}

#ifdef HAVE_LFORTRAN_LLVM

void section(const std::string &s)
{
    std::cout << color(LFortran::style::bold) << color(LFortran::fg::blue) << s << color(LFortran::style::reset) << color(LFortran::fg::reset) << std::endl;
}


int prompt(bool verbose)
{
    Terminal term(true, false);
    std::cout << "Interactive Fortran. Experimental prototype, not ready for end users." << std::endl;
    std::cout << "  * Use Ctrl-D to exit" << std::endl;
    std::cout << "  * Use Enter to submit" << std::endl;
    std::cout << "  * Use Alt-Enter to make a new line" << std::endl;
    std::cout << "    - Editing (Keys: Left, Right, Home, End, Backspace, Delete)" << std::endl;
    std::cout << "    - History (Keys: Up, Down)" << std::endl;

    Allocator al(64*1024*1024);
    LFortran::FortranEvaluator e;

    std::vector<std::string> history;
    while (true) {
        std::string input = prompt0(term, ">>> ", history);
        if (input.size() == 1 && input[0] == CTRL_KEY('d')) {
            std::cout << std::endl;
            std::cout << "Exiting." << std::endl;
            return 0;
        }

        if (verbose) {
            section("Input:");
            std::cout << input << std::endl;
        }

        LFortran::FortranEvaluator::EvalResult r;

        try {
            LFortran::FortranEvaluator::Result<LFortran::FortranEvaluator::EvalResult>
            res = e.evaluate(input, verbose);
            if (res.ok) {
                r = res.result;
            } else {
                std::cerr << e.format_error(res.error, input);
                continue;
            }
        } catch (const LFortran::LFortranException &e) {
            std::cout << "Other LFortran exception: " << e.msg() << std::endl;
            continue;
        }

        if (verbose) {
            section("AST:");
            std::cout << r.ast  << std::endl;
            section("ASR:");
            std::cout << r.asr << std::endl;
            section("LLVM IR:");
            std::cout << r.llvm_ir << std::endl;
        }

        switch (r.type) {
            case (LFortran::FortranEvaluator::EvalResult::integer) : {
                if (verbose) std::cout << "Return type: integer" << std::endl;
                if (verbose) section("Result:");
                std::cout << r.i << std::endl;
                break;
            }
            case (LFortran::FortranEvaluator::EvalResult::real) : {
                if (verbose) std::cout << "Return type: real" << std::endl;
                if (verbose) section("Result:");
                std::cout << r.f << std::endl;
                break;
            }
            case (LFortran::FortranEvaluator::EvalResult::statement) : {
                if (verbose) {
                    std::cout << "Return type: none" << std::endl;
                    section("Result:");
                    std::cout << "(statement)" << std::endl;
                }
                break;
            }
            case (LFortran::FortranEvaluator::EvalResult::none) : {
                if (verbose) {
                    std::cout << "Return type: none" << std::endl;
                    section("Result:");
                    std::cout << "(nothing to execute)" << std::endl;
                }
                break;
            }
            default : throw LFortran::LFortranException("Return type not supported");
        }
    }
    return 0;
}
#endif

int emit_tokens(const std::string &infile)
{
    std::string input = read_file(infile);
    // Src -> Tokens
    Allocator al(64*1024*1024);
    std::vector<int> toks;
    std::vector<LFortran::YYSTYPE> stypes;
    try {
        toks = LFortran::tokens(input, &stypes);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    }

    for (size_t i=0; i < toks.size(); i++) {
        std::cout << LFortran::pickle(toks[i], stypes[i]) << std::endl;
    }
    return 0;
}

int emit_ast(const std::string &infile, bool colors)
{
    std::string input = read_file(infile);
    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    std::cout << LFortran::pickle(*ast, colors) << std::endl;
    return 0;
}

int emit_ast_f90(const std::string &infile, bool colors)
{
    std::string input = read_file(infile);
    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> Source
    std::string source = LFortran::ast_to_src(*ast, colors);

    std::cout << source;
    return 0;
}

int format(const std::string &file, bool inplace, bool color, int indent,
    bool indent_unit)
{
    if (inplace) color = false;
    std::string input = read_file(file);
    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> Source
    std::string source = LFortran::ast_to_src(*ast, color,
        indent, indent_unit);

    if (inplace) {
        std::ofstream out;
        out.open(file);
        out << source;
    } else {
        std::cout << source;
    }

    return 0;
}

int emit_asr(const std::string &infile, bool colors,
    const std::vector<ASRPass> &passes)
{
    std::string input = read_file(infile);

    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> ASR
    LFortran::ASR::TranslationUnit_t* asr;
    try {
        // FIXME: For now we only transform the first node in the list:
        asr = LFortran::ast_to_asr(al, *ast);
    } catch (const LFortran::SemanticError &e) {
        std::cerr << "Semantic error: " << e.msg() << std::endl;
        return 2;
    }

    for (size_t i=0; i < passes.size(); i++) {
        switch (passes[i]) {
            case (ASRPass::do_loops) : {
                LFortran::pass_replace_do_loops(al, *asr);
                break;
            }
            case (ASRPass::global_stmts) : {
                LFortran::pass_wrap_global_stmts_into_function(al, *asr, "f");
                break;
            }
            default : throw LFortran::LFortranException("Pass not implemened");
        }
    }

    std::cout << LFortran::pickle(*asr, colors) << std::endl;
    return 0;
}

int emit_cpp(const std::string &infile)
{
    std::string input = read_file(infile);

    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> ASR
    LFortran::ASR::TranslationUnit_t* asr = LFortran::ast_to_asr(al, *ast);

    // ASR -> CPP
    std::string cpp;
    cpp = LFortran::asr_to_cpp(*asr);

    std::cout << cpp;
    return 0;
}


#ifdef HAVE_LFORTRAN_LLVM

int save_mod_files(const LFortran::ASR::TranslationUnit_t &u)
{
    for (auto &item : u.m_global_scope->scope) {
        if (LFortran::ASR::is_a<LFortran::ASR::Module_t>(*item.second)) {
            LFortran::ASR::Module_t *m = LFortran::ASR::down_cast<LFortran::ASR::Module_t>(item.second);
            std::string modfile = std::string(m->m_name) + ".mod";
            std::string cmd = "touch " + modfile;
            int err = system(cmd.c_str());
            if (err) {
                std::cout << "The command '" + cmd + "' failed." << std::endl;
                return 11;
            }
        }
    }
    return 0;
}

int emit_llvm(const std::string &infile)
{
    std::string input = read_file(infile);

    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> ASR
    LFortran::ASR::TranslationUnit_t* asr = LFortran::ast_to_asr(al, *ast);

    // ASR -> LLVM
    LFortran::LLVMEvaluator e;
    std::unique_ptr<LFortran::LLVMModule> m;
    try {
        m = LFortran::asr_to_llvm(*asr, e.get_context(), al);
    } catch (const LFortran::CodeGenError &e) {
        std::cerr << "Code generation error: " << e.msg() << std::endl;
        return 5;
    }

    std::cout << m->str() << std::endl;
    return 0;
}

int compile_to_object_file(const std::string &infile, const std::string &outfile,
        bool assembly=false,
        bool show_stacktrace=false)
{
    std::string input = read_file(infile);

    LFortran::FortranEvaluator fe;
    LFortran::ASR::TranslationUnit_t* asr;


    // Src -> AST
    LFortran::FortranEvaluator::Result<LFortran::ASR::TranslationUnit_t*>
    result = fe.get_asr2(input);
    if (result.ok) {
        asr = result.result;
    } else {
        if (show_stacktrace) {
            std::cerr << fe.error_stacktrace(result.error);
        }
        std::cerr << fe.format_error(result.error, input);
        return 1;
    }

    // Save .mod files
    {
        int err = save_mod_files(*asr);
        if (err) return err;
    }

    // ASR -> LLVM
    LFortran::LLVMEvaluator e;
    std::unique_ptr<LFortran::LLVMModule> m;
    Allocator al(64*1024*1024);
    try {
        m = LFortran::asr_to_llvm(*asr, e.get_context(), al);
    } catch (const LFortran::CodeGenError &e) {
        if (show_stacktrace) {
            std::cerr << e.stacktrace();
        }
        std::cerr << "Code generation error: " << e.msg() << std::endl;
        return 5;
    }

    // LLVM -> Machine code (saves to an object file)
    if (assembly) {
        e.save_asm_file(*(m->m_m), outfile);
    } else {
        e.save_object_file(*(m->m_m), outfile);
    }

    return 0;
}

int compile_to_assembly_file(const std::string &infile, const std::string &outfile)
{
    return compile_to_object_file(infile, outfile, true, false);
}
#endif


int compile_to_binary_x86(const std::string &infile, const std::string &outfile,
        bool time_report)
{
    int time_file_read=0;
    int time_src_to_ast=0;
    int time_ast_to_asr=0;
    int time_asr_to_x86=0;

    std::string input;
    Allocator al(64*1024*1024); // Allocate 64 MB
    LFortran::AST::TranslationUnit_t* ast;
    LFortran::ASR::TranslationUnit_t* asr;

    {
        auto t1 = std::chrono::high_resolution_clock::now();
        input = read_file(infile);
        auto t2 = std::chrono::high_resolution_clock::now();
        time_file_read = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    }

    // Src -> AST
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        try {
            ast = LFortran::parse2(al, input);
        } catch (const LFortran::TokenizerError &e) {
            std::cerr << "Tokenizing error: " << e.msg() << std::endl;
            return 1;
        } catch (const LFortran::ParserError &e) {
            std::cerr << "Parsing error: " << e.msg() << std::endl;
            return 2;
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        time_src_to_ast = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    }

    // AST -> ASR
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        asr = LFortran::ast_to_asr(al, *ast);
        auto t2 = std::chrono::high_resolution_clock::now();
        time_ast_to_asr = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    }

    // ASR -> x86 machine code
    {
        auto t1 = std::chrono::high_resolution_clock::now();
        LFortran::asr_to_x86(*asr, al, outfile, time_report);
        auto t2 = std::chrono::high_resolution_clock::now();
        time_asr_to_x86 = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    }

    if (time_report) {
        std::cout << "Allocator usage of last chunk (MB): "
            << al.size_current() / (1024. * 1024) << std::endl;
        std::cout << "Allocator chunks: " << al.num_chunks() << std::endl;
        std::cout << std::endl;
        std::cout << "Time report:" << std::endl;
        std::cout << "File reading:" << std::setw(5) << time_file_read << std::endl;
        std::cout << "Src -> AST:  " << std::setw(5) << time_src_to_ast << std::endl;
        std::cout << "AST -> ASR:  " << std::setw(5) << time_ast_to_asr << std::endl;
        std::cout << "ASR -> x86:  " << std::setw(5) << time_asr_to_x86 << std::endl;
        int total = time_file_read + time_src_to_ast + time_ast_to_asr
                + time_asr_to_x86;
        std::cout << "Total:       " << std::setw(5) << total << std::endl;
    }

    return 0;
}


int compile_to_object_file_cpp(const std::string &infile,
        const std::string &outfile,
        bool assembly, bool kokkos)
{
    std::string input = read_file(infile);

    // Src -> AST
    Allocator al(64*1024*1024);
    LFortran::AST::TranslationUnit_t* ast;
    try {
        ast = LFortran::parse2(al, input);
    } catch (const LFortran::TokenizerError &e) {
        std::cerr << "Tokenizing error: " << e.msg() << std::endl;
        return 1;
    } catch (const LFortran::ParserError &e) {
        std::cerr << "Parsing error: " << e.msg() << std::endl;
        return 2;
    }

    // AST -> ASR
    LFortran::ASR::TranslationUnit_t* asr = LFortran::ast_to_asr(al, *ast);

    // ASR -> C++
    std::string src;
    try {
        src = LFortran::asr_to_cpp(*asr);
    } catch (const LFortran::CodeGenError &e) {
        std::cerr << "Code generation error: " << e.msg() << std::endl;
        return 5;
    }

    // C++ -> Machine code (saves to an object file)
    if (assembly) {
        throw LFortran::LFortranException("Not implemented");
    } else {
        std::string cppfile = outfile + ".tmp.cpp";
        {
            std::ofstream out;
            out.open(cppfile);
            out << src;
        }

        std::string CXX = "g++";
        std::string options;
        if (kokkos) {
            std::string kokkos_dir = get_kokkos_dir();
            options += "-fopenmp -I" + kokkos_dir + "/include";
        }
        std::string cmd = CXX + " " + options + " -o " + outfile + " -c " + cppfile;
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 11;
        }
    }

    return 0;
}

// infile is an object file
// outfile will become the executable
int link_executable(const std::string &infile, const std::string &outfile,
    const std::string &runtime_library_dir, Backend backend,
    bool static_executable, bool kokkos)
{
    /*
    The `gcc` line for dynamic linking that is constructed below:

    gcc -o $outfile $infile \
        -Lsrc/runtime -Wl,-rpath=src/runtime -llfortran_runtime

    is equivalent to the following:

    ld -o $outfile $infile \
        -Lsrc/runtime -rpath=src/runtime -llfortran_runtime \
        -dynamic-linker /lib64/ld-linux-x86-64.so.2  \
        /usr/lib/x86_64-linux-gnu/Scrt1.o /usr/lib/x86_64-linux-gnu/libc.so

    and this for static linking:

    gcc -static -o $outfile $infile \
        -Lsrc/runtime -Wl,-rpath=src/runtime -llfortran_runtime_static

    is equivalent to:

    ld -o $outfile $infile \
        -Lsrc/runtime -rpath=src/runtime -llfortran_runtime_static \
        /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o \
        /usr/lib/x86_64-linux-gnu/libc.a \
        /usr/lib/gcc/x86_64-linux-gnu/7/libgcc_eh.a \
        /usr/lib/x86_64-linux-gnu/libc.a \
        /usr/lib/gcc/x86_64-linux-gnu/7/libgcc.a \
        /usr/lib/x86_64-linux-gnu/crtn.o

    This was tested on Ubuntu 18.04.

    The `gcc` and `ld` approaches are equivalent except:

    1. The `gcc` command knows how to find and link the `libc` library,
       while in `ld` we must do that manually
    2. For dynamic linking, we must also specify the dynamic linker for `ld`

    Notes:

    * We can use `lld` to do the linking via the `ld` approach, so `ld` is
      preferable if we can mitigate the issues 1. and 2.
    * If we ship our own libc (such as musl), then we know how to find it
      and link it, which mitigates the issue 1.
    * If we link `musl` statically, then issue 2. does not apply.
    * If we link `musl` dynamically, then we have to find the dynamic
      linker (doable), which mitigates the issue 2.

    One way to find the default dynamic linker is by:

        $ readelf -e /bin/bash | grep ld-linux
            [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]

    There are probably simpler ways.


    */
    if (backend == Backend::llvm) {
        std::string CC = "gcc";
        std::string base_path = runtime_library_dir;
        std::string options;
        std::string runtime_lib = "lfortran_runtime";
        if (static_executable) {
            options += " -static ";
            runtime_lib = "lfortran_runtime_static";
        }
        std::string cmd = CC + options + " -o " + outfile + " " + infile + " -L"
            + base_path + " -Wl,-rpath=" + base_path + " -l" + runtime_lib + " -lm";
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 10;
        }
        return 0;
    } else if (backend == Backend::cpp) {
        std::string CXX = "g++";
        std::string options, post_options;
        if (static_executable) {
            options += " -static ";
        }
        if (kokkos) {
            std::string kokkos_dir = get_kokkos_dir();
            options += " -fopenmp ";
            post_options += kokkos_dir + "/lib/libkokkoscontainers.a "
                + kokkos_dir + "/lib/libkokkoscore.a -ldl";
        }
        std::string cmd = CXX + options + " -o " + outfile + " " + infile
            + " " + post_options + " -lm";
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 10;
        }
        return 0;
    } else if (backend == Backend::x86) {
        std::string cmd = "cp " + infile + " " + outfile;
        int err = system(cmd.c_str());
        if (err) {
            std::cout << "The command '" + cmd + "' failed." << std::endl;
            return 10;
        }
        return 0;
    } else {
        LFORTRAN_ASSERT(false);
        return 1;
    }
}

void get_executable_path(std::string &executable_path, int &dirname_length)
{
    int length;

    length = wai_getExecutablePath(NULL, 0, &dirname_length);
    if (length > 0) {
        std::string path(length+1, '\0');
        wai_getExecutablePath(&path[0], length, &dirname_length);
        executable_path = path;
    } else {
        throw LFortran::LFortranException("Cannot determine executable path.");
    }
}

std::string get_runtime_library_dir()
{
    char *env_p = std::getenv("LFORTRAN_RUNTIME_LIBRARY_DIR");
    if (env_p) return env_p;

    std::string path;
    int dirname_length;
    get_executable_path(path, dirname_length);
    std::string dirname = path.substr(0,dirname_length);
    if (endswith(dirname, "src/bin")) {
        // Development version
        return dirname + "/../runtime";
    } else {
        // Installed version
        return dirname + "/../share/lfortran/lib/";
    }
}

} // anonymous namespace

int main(int argc, char *argv[])
{
#if defined(HAVE_LFORTRAN_STACKTRACE)
    LFortran::print_stack_on_segfault();
#endif
    try {
        int dirname_length;
        get_executable_path(LFortran::binary_executable_path, dirname_length);

        std::string runtime_library_dir = get_runtime_library_dir();
        Backend backend;

        bool arg_S = false;
        bool arg_c = false;
        bool arg_v = false;
        bool arg_E = false;
        std::vector<std::string> arg_I;
        bool arg_cpp = false;
        std::string arg_o;
        std::string arg_file;
        bool arg_version = false;
        bool show_tokens = false;
        bool show_ast = false;
        bool show_asr = false;
        bool show_ast_f90 = false;
        std::string arg_pass;
        bool arg_no_color = false;
        bool show_llvm = false;
        bool show_cpp = false;
        bool show_asm = false;
        bool time_report = false;
        bool show_stacktrace = false;
        bool static_link = false;
        std::string arg_backend = "llvm";
        std::string arg_kernel_f;

        std::string arg_fmt_file;
        int arg_fmt_indent = 4;
        bool arg_fmt_indent_unit = false;
        bool arg_fmt_inplace = false;
        bool arg_fmt_no_color = false;

        std::string arg_mod_file;
        bool arg_mod_show_asr = false;
        bool arg_mod_no_color = false;

        CLI::App app{"LFortran: modern interactive LLVM-based Fortran compiler"};
        // Standard options compatible with gfortran, gcc or clang
        // We follow the established conventions
        app.add_option("file", arg_file, "Source file");
        app.add_flag("-S", arg_S, "Emit assembly, do not assemble or link");
        app.add_flag("-c", arg_c, "Compile and assemble, do not link");
        app.add_option("-o", arg_o, "Specify the file to place the output into");
        app.add_flag("-v", arg_v, "Be more verbose");
        app.add_flag("-E", arg_E, "Preprocess only; do not compile, assemble or link");
        app.add_option("-I", arg_I, "Include path");
        app.add_flag("--version", arg_version, "Display compiler version information");

        // LFortran specific options
        app.add_flag("--cpp", arg_cpp, "Enable preprocessing");
        app.add_flag("--show-tokens", show_tokens, "Show tokens for the given file and exit");
        app.add_flag("--show-ast", show_ast, "Show AST for the given file and exit");
        app.add_flag("--show-asr", show_asr, "Show ASR for the given file and exit");
        app.add_flag("--show-ast-f90", show_ast_f90, "Show Fortran from AST for the given file and exit");
        app.add_flag("--no-color", arg_no_color, "Turn off colored AST/ASR");
        app.add_option("--pass", arg_pass, "Apply the ASR pass and show ASR (implies --show-asr)");
        app.add_flag("--show-llvm", show_llvm, "Show LLVM IR for the given file and exit");
        app.add_flag("--show-cpp", show_cpp, "Show C++ translation source for the given file and exit");
        app.add_flag("--show-asm", show_asm, "Show assembly for the given file and exit");
        app.add_flag("--show-stacktrace", show_stacktrace, "Show internal stacktrace on compiler errors");
        app.add_flag("--time-report", time_report, "Show compilation time report");
        app.add_flag("--static", static_link, "Create a static executable");
        app.add_option("--backend", arg_backend, "Select a backend (llvm, cpp, x86)", true);

        /*
        * Subcommands:
        */

        // fmt
        CLI::App &fmt = *app.add_subcommand("fmt", "Format Fortran source files.");
        fmt.add_option("file", arg_fmt_file, "Fortran source file to format")->required();
        fmt.add_flag("-i", arg_fmt_inplace, "Modify <file> in-place (instead of writing to stdout)");
        fmt.add_option("--spaces", arg_fmt_indent, "Number of spaces to use for indentation", true);
        fmt.add_flag("--indent-unit", arg_fmt_indent_unit, "Indent contents of sub / fn / prog / mod");
        fmt.add_flag("--no-color", arg_fmt_no_color, "Turn off color when writing to stdout");

        // kernel
        CLI::App &kernel = *app.add_subcommand("kernel", "Run in Jupyter kernel mode.");
        kernel.add_option("-f", arg_kernel_f, "The kernel connection file")->required();

        // mod
        CLI::App &mod = *app.add_subcommand("mod", "Fortran mod file utilities.");
        mod.add_option("file", arg_mod_file, "Mod file (*.mod)")->required();
        mod.add_flag("--show-asr", arg_mod_show_asr, "Show ASR for the module");
        mod.add_flag("--no-color", arg_mod_no_color, "Turn off colored ASR");


        app.get_formatter()->column_width(25);
        app.require_subcommand(0, 1);
        CLI11_PARSE(app, argc, argv);

        if (arg_version) {
            std::string version = LFORTRAN_VERSION;
            std::cout << "LFortran version: " << version << std::endl;
            return 0;
        }

        if (fmt) {
            return format(arg_fmt_file, arg_fmt_inplace, !arg_fmt_no_color,
                arg_fmt_indent, arg_fmt_indent_unit);
        }

        if (kernel) {
#ifdef HAVE_LFORTRAN_XEUS
            return LFortran::run_kernel(arg_kernel_f);
#else
            std::cerr << "The kernel subcommand requires LFortran to be compiled with XEUS support. Recompile with `WITH_XEUS=yes`." << std::endl;
            return 1;
#endif
        }

        if (mod) {
            if (arg_mod_show_asr) {
                Allocator al(1024*1024);
                LFortran::ASR::TranslationUnit_t *asr;
                asr = LFortran::mod_to_asr(al, arg_mod_file);
                std::cout << LFortran::pickle(*asr, !arg_mod_no_color) << std::endl;
                return 0;
            }
            return 0;
        }

        if (arg_backend == "llvm") {
            backend = Backend::llvm;
        } else if (arg_backend == "cpp") {
            backend = Backend::cpp;
        } else if (arg_backend == "x86") {
            backend = Backend::x86;
        } else {
            std::cerr << "The backend must be one of: llvm, cpp, x86." << std::endl;
            return 1;
        }

        if (arg_file.size() == 0) {
#ifdef HAVE_LFORTRAN_LLVM
            return prompt(arg_v);
#else
            std::cerr << "Interactive prompt requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
            return 1;
#endif
        }

        std::string outfile;
        std::string basename;
        basename = remove_extension(arg_file);
        basename = remove_path(basename);
        if (arg_o.size() > 0) {
            outfile = arg_o;
        } else if (arg_S) {
            outfile = basename + ".s";
        } else if (arg_c) {
            outfile = basename + ".o";
        } else if (show_tokens) {
            outfile = basename + ".tokens";
        } else if (show_ast) {
            outfile = basename + ".ast";
        } else if (show_asr) {
            outfile = basename + ".asr";
        } else if (show_llvm) {
            outfile = basename + ".ll";
        } else {
            outfile = "a.out";
        }

        if (arg_cpp) {
            std::string file_cpp = arg_file + ".preprocessed";
            std::string cmd = "gfortran -cpp -E " + arg_file + " -o "
                + file_cpp;
            int err = system(cmd.c_str());
            if (err) {
                std::cout << "The command '" + cmd + "' failed." << std::endl;
                return 11;
            }
            std::string file_cpp2 = file_cpp + "2";
            std::string input = read_file(file_cpp);
            std::string output = LFortran::fix_continuation(input);
            {
                std::ofstream out;
                out.open(file_cpp2);
                out << output;
            }
            arg_file = file_cpp2;
        }

        if (arg_E) {
            return 0;
        }


        if (show_tokens) {
            return emit_tokens(arg_file);
        }
        if (show_ast) {
            return emit_ast(arg_file, !arg_no_color);
        }
        if (show_ast_f90) {
            return emit_ast_f90(arg_file, !arg_no_color);
        }
        std::vector<ASRPass> passes;
        if (arg_pass != "") {
            if (arg_pass == "do_loops") {
                passes.push_back(ASRPass::do_loops);
            } else if (arg_pass == "global_stmts") {
                passes.push_back(ASRPass::global_stmts);
            } else {
                std::cerr << "Pass must be one of: do_loops, global_stmts" << std::endl;
                return 1;
            }
            show_asr = true;
        }
        if (show_asr) {
            return emit_asr(arg_file, !arg_no_color, passes);
        }
        if (show_llvm) {
#ifdef HAVE_LFORTRAN_LLVM
            return emit_llvm(arg_file);
#else
            std::cerr << "The --show-llvm option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
            return 1;
#endif
        }
        if (show_cpp) {
            return emit_cpp(arg_file);
        }
        if (arg_S) {
            if (backend == Backend::llvm) {
#ifdef HAVE_LFORTRAN_LLVM
                return compile_to_assembly_file(arg_file, outfile);
#else
                std::cerr << "The -S option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
                return 1;
#endif
            } else if (backend == Backend::cpp) {
                std::cerr << "The C++ backend does not work with the -S option yet." << std::endl;
                return 1;
            } else {
                LFORTRAN_ASSERT(false);
            }
        }
        if (arg_c) {
            if (backend == Backend::llvm) {
#ifdef HAVE_LFORTRAN_LLVM
                return compile_to_object_file(arg_file, outfile, false,
                    show_stacktrace);
#else
                std::cerr << "The -c option requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
                return 1;
#endif
            } else if (backend == Backend::cpp) {
                return compile_to_object_file_cpp(arg_file, outfile, false, true);
            } else if (backend == Backend::x86) {
                return compile_to_binary_x86(arg_file, outfile, time_report);
            } else {
                throw LFortran::LFortranException("Unsupported backend.");
            }
        }

        if (endswith(arg_file, ".f90")) {
            if (backend == Backend::x86) {
                return compile_to_binary_x86(arg_file, outfile,
                        time_report);
            }
            std::string tmp_o = outfile + ".tmp.o";
            int err;
            if (backend == Backend::llvm) {
#ifdef HAVE_LFORTRAN_LLVM
                err = compile_to_object_file(arg_file, tmp_o, false,
                    show_stacktrace);
#else
                std::cerr << "Compiling Fortran files to object files requires the LLVM backend to be enabled. Recompile with `WITH_LLVM=yes`." << std::endl;
                return 1;
#endif
            } else if (backend == Backend::cpp) {
                err = compile_to_object_file_cpp(arg_file, tmp_o, false, true);
            } else {
                throw LFortran::LFortranException("Backend not supported");
            }
            if (err) return err;
            return link_executable(tmp_o, outfile, runtime_library_dir,
                    backend, static_link, true);
        } else {
            return link_executable(arg_file, outfile, runtime_library_dir,
                    backend, static_link, true);
        }
    } catch(const LFortran::LFortranException &e) {
        std::cerr << e.stacktrace();
        std::cerr << e.name() + ": " << e.msg() << std::endl;
        return 1;
    } catch(const std::runtime_error &e) {
        std::cerr << "runtime_error: " << e.what() << std::endl;
        return 1;
    } catch(const std::exception &e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
        return 1;
    } catch(...) {
        std::cerr << "Unknown Exception" << std::endl;
        return 1;
    }
    return 0;
}
