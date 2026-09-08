import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def conditional_block(text, marker):
    start = text.index(marker)
    depth = 0
    lines = []
    for line in text[start:].splitlines(keepends=True):
        lines.append(line)
        if re.match(r'\s*if\s*\(', line):
            depth += 1
        elif re.match(r'\s*endif\s*\(', line):
            depth -= 1
            if depth == 0:
                return ''.join(lines)
    raise AssertionError('Unterminated SDK CMake conditional')


def run(command, expect_success=True):
    env = os.environ.copy()
    env['MSBUILDDISABLENODEREUSE'] = '1'
    env.setdefault('_CL_', '/MP1')
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                            errors='replace', env=env, timeout=180)
    output = result.stdout + result.stderr
    if expect_success and result.returncode != 0:
        raise AssertionError('Command failed: ' + ' '.join(map(str, command)) + '\n' + output)
    return result.returncode, output


def prepare(root, install_rules, delay_rules, runtime_files):
    source = root / 'source'
    build = root / 'build'
    source.mkdir(parents=True)
    (source / 'probe.c').write_text('__declspec(dllexport) int sdk_install_fixture(void) { return 0; }\n')
    for name in ('phonon.h', 'phonon_version.h', 'phonon_interfaces.h'):
        (source / name).write_text('SDK install test fixture\n')
    if runtime_files:
        for config in ('debug', 'release'):
            directory = source / 'deps/trueaudionext/bin/windows-x64' / config
            directory.mkdir(parents=True)
            for name in ('TrueAudioNext.dll', 'GPUUtilities.dll'):
                (directory / name).write_text(name + ':' + config + '\n')
    project = '''cmake_minimum_required(VERSION 3.20)
project(SdkInstallFixture LANGUAGES C)
if (NOT MSVC)
    message(FATAL_ERROR "SDK install regression tests require MSVC")
endif()
set(IPL_OS_WINDOWS TRUE)
set(IPL_CPU_X64 TRUE)
set(BUILD_SHARED_LIBS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(phonon SHARED probe.c)
target_compile_options(phonon PRIVATE /Zi)
target_link_options(phonon PRIVATE /DEBUG)
target_link_libraries(phonon PRIVATE delayimp)
configure_file(phonon_version.h "${CMAKE_CURRENT_BINARY_DIR}/phonon_version.h" COPYONLY)
function(get_bin_subdir variable)
    set(${variable} windows-x64 PARENT_SCOPE)
endfunction()
'''
    project += delay_rules
    project += '''get_target_property(delay_options phonon LINK_OPTIONS)
get_target_property(delay_flags phonon LINK_FLAGS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/delay-options.txt" "${delay_options};${delay_flags}")
'''
    project += install_rules
    (source / 'CMakeLists.txt').write_text(project, encoding='utf-8')
    return source, build


def configure(args, source, build, enabled):
    run([args.cmake, '-S', str(source), '-B', str(build), '-G', 'Visual Studio 17 2022',
         '-A', 'x64', '-DSTEAMAUDIO_ENABLE_TRUEAUDIONEXT=' + ('ON' if enabled else 'OFF')])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk-root', type=Path, required=True)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--work-dir', type=Path, default=Path(tempfile.gettempdir()))
    args = parser.parse_args()
    sdk = args.sdk_root / 'core/src/core/CMakeLists.txt'
    text = sdk.read_text(encoding='utf-8')
    install_rules = text.split('\n# INSTALL\n#\n', 1)[1]
    delay_rules = conditional_block(text, 'if (IPL_OS_WINDOWS AND IPL_CPU_X64 AND BUILD_SHARED_LIBS)')
    configurations = ('RelWithDebInfo', 'Release', 'Debug', 'MinSizeRel')
    with tempfile.TemporaryDirectory(prefix='sa-sdk-install-', dir=args.work_dir) as temporary:
        root = Path(temporary)
        for enabled in (False, True):
            label = 'enabled' if enabled else 'disabled'
            source, build = prepare(root / label, install_rules, delay_rules, enabled)
            configure(args, source, build, enabled)
            for config in configurations:
                run([args.cmake, '--build', str(build), '--config', config, '--target', 'phonon', '--parallel', '2'])
                destination = root / label / ('installed-' + config)
                run([args.cmake, '--install', str(build), '--config', config, '--prefix', str(destination)])
                for relative in ('lib/windows-x64/phonon.dll', 'lib/windows-x64/phonon.lib',
                                 'symbols/windows-x64/phonon.pdb', 'include/phonon.h',
                                 'include/phonon_version.h', 'include/phonon_interfaces.h'):
                    assert (destination / relative).is_file(), relative
                for name in ('TrueAudioNext.dll', 'GPUUtilities.dll'):
                    installed = destination / 'lib/windows-x64' / name
                    assert installed.exists() == enabled, str(installed)
                    if enabled:
                        expected = name + ':' + ('debug' if config == 'Debug' else 'release') + '\n'
                        assert installed.read_text() == expected, str(installed)
                print('PASS: TAN ' + label + ', install ' + config, flush=True)
            options = (build / 'delay-options.txt').read_text().lower()
            assert '/delayload:opencl.dll' in options
            for name in ('trueaudionext.dll', 'gpuutilities.dll'):
                assert ('/delayload:' + name in options) == enabled, options
            print('PASS: TAN ' + label + ', delay-load flags', flush=True)
        source, build = prepare(root / 'missing-runtime', install_rules, delay_rules, False)
        configure(args, source, build, True)
        run([args.cmake, '--build', str(build), '--config', 'RelWithDebInfo', '--target', 'phonon', '--parallel', '2'])
        code, output = run([args.cmake, '--install', str(build), '--config', 'RelWithDebInfo',
                            '--prefix', str(root / 'missing-runtime/installed')], expect_success=False)
        assert code != 0 and 'TrueAudioNext.dll' in output, output
        print('PASS: enabled TAN still rejects missing runtime DLLs', flush=True)
    print('SDK install regression checks passed', flush=True)


if __name__ == '__main__':
    main()
