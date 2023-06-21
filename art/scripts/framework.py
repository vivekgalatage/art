from string import Template

import argparse
import os
import shlex
import shutil
import subprocess

top_level_header_content = """\
#ifndef __OBJC__
#error "This file MUST be included in ObjC compilation context only!"
#endif

"""


modulemap_content_template = """\
framework module $module_name {
  umbrella header "$umbrella_header"

  export *
  module * { export * }
}
"""


def generate_top_level_header(framework_name, header_files):
  header_files.sort()
  return top_level_header_content + \
      f"#import <{framework_name}/" +\
      f">\n#import <{framework_name}/".join(header_files) + ">\n"


def write_header_files(
    framework_name,
    framework_name_without_extension,
    source_root,
    header_files,
    header_path):
  top_level_header_file_name = framework_name_without_extension + ".h"
  top_level_header_file_path = os.path.join(
      header_path, top_level_header_file_name)

  with open(top_level_header_file_path, "w") as top_level_header_file:
    top_level_header_file.write(
        generate_top_level_header(
            framework_name_without_extension, header_files))

  for header_file in header_files:
    header_name = os.path.basename(header_file)
    header_dir = os.path.join(header_path, os.path.dirname(header_file))
    os.makedirs(header_dir, exist_ok=True)
    src_file = os.path.join(source_root, header_file)
    dst_file = os.path.join(header_dir, header_name)
    shutil.copy2(src_file, dst_file)

  return top_level_header_file_name


def write_modulemap_file(
    framework_name_without_extension,
    top_level_header_file_name,
    modules_path):
  modulemap_template = Template(modulemap_content_template)
  modulemap_file_path = os.path.join(modules_path, "module.modulemap")
  with open(modulemap_file_path, "w") as modulemap_file:
    modulemap_file.write(modulemap_template.substitute(
        module_name=framework_name_without_extension,
        umbrella_header=top_level_header_file_name))


def make_framework_folder_with_symlinks(framework_path, framework_name):
  if os.path.isdir(framework_path):
    shutil.rmtree(framework_path)

  os.makedirs(framework_path, exist_ok=True)

  # Create the necessary framework directory structure
  versions_dir = os.path.join(framework_path, "Versions/A")
  headers_dir = os.path.join(framework_path, "Versions/A/Headers")
  modules_dir = os.path.join(framework_path, "Versions/A/Modules")

  os.makedirs(headers_dir, exist_ok=True)
  os.makedirs(modules_dir, exist_ok=True)

  if not os.path.islink(framework_path + "/Versions/Current"):
    os.symlink(
        "A",
        framework_path + "/Versions/Current",
        target_is_directory=True)

  if not os.path.islink(framework_path + "/Headers"):
    os.symlink(
        "Versions/Current/Headers",
        framework_path + "/Headers",
        target_is_directory=True)

  if not os.path.islink(framework_path + "/Modules"):
    os.symlink(
        "Versions/Current/Modules",
        framework_path + "/Modules",
        target_is_directory=True)


def write_dylibs(
    framework_path,
    framework_name,
    framework_dylib,
    signing_key):
  framework_dylib_dest = os.path.join(
      framework_path,
      "Versions/Current",
      framework_name)
  shutil.copy2(framework_dylib, framework_dylib_dest)
  os.symlink(
      f"Versions/Current/{framework_name}",
      os.path.join(framework_path, framework_name))

  subprocess.run([
      "install_name_tool",
      "-id",
      f"@rpath/{framework_name}.framework/{framework_name}",
      framework_dylib_dest])

  dylib_name = os.path.basename(framework_dylib)

  subprocess.run([
      "install_name_tool",
      "-change",
      f"@rpath/{dylib_name}",
      f"@rpath/{framework_path}/{framework_name}",
      framework_dylib_dest])


  if signing_key != None:
    subprocess.run(["codesign", "-s", signing_key, framework_dylib_dest])

def build(args, unknown_args):
  with open(args.input_files) as f:
    input_files = shlex.split(f.read())

  header_files = [item for item in input_files if item.endswith(".h")]

  source_root = args.source_root

  framework_name = args.framework_name
  framework_path = os.path.join(args.out_dir, args.framework_name)
  framework_name_without_extension = framework_name.split(".")[0]

  make_framework_folder_with_symlinks(framework_path, framework_name)

  header_path = os.path.join(framework_path, "Headers")
  modules_path = os.path.join(framework_path, "Modules")

  top_level_header_file_name = write_header_files(
                                  framework_name,
                                  framework_name_without_extension,
                                  source_root,
                                  header_files,
                                  header_path)

  write_modulemap_file(
      framework_name_without_extension,
      top_level_header_file_name,
      modules_path)

  write_dylibs(
      framework_path,
      framework_name_without_extension,
      args.framework_dylib,
      args.signing_key)


def parse_cli_args():
  parser = argparse.ArgumentParser(prog="objcpp")

  subparsers = parser.add_subparsers(dest="command")

  build_command = subparsers.add_parser("build")
  build_command.add_argument("--framework-name")
  build_command.add_argument("--framework-dylib")
  build_command.add_argument("--source-root")
  build_command.add_argument("--out-dir")
  build_command.add_argument("--input-files")
  build_command.add_argument("--signing-key")

  build_command.set_defaults(func=build)

  args, unknown_args = parser.parse_known_args()
  if hasattr(args, "func"):
    args.func(args, unknown_args)


cli_args = parse_cli_args()
