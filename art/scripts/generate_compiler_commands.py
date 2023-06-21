import argparse
import json
import os
import subprocess

def generate(args):
  current_dir = os.getcwd()
  output_dir = os.path.abspath(os.path.join(current_dir, args.out_dir))
  json_file = os.path.abspath(os.path.join(current_dir, args.json))

  os.chdir(output_dir)
  ninja_compdb = [
    "ninja",
    "-C", output_dir,
    "-t", "compdb", "cc", "cxx", "objc", "objcxx",
  ]

  ninja_commands = [
    "ninja",
    "-C", output_dir,
    "-t", "commands",
  ]
  try:
    result = subprocess.run(ninja_compdb, capture_output=True, text=True, check=True)
    json_array = json.loads(result.stdout)
    result = subprocess.run(ninja_commands, capture_output=True, text=True, check=True)
    objcpp_commands = [item for item in result.stdout.split("\n") if "objcpp.py compile" in item]
    for command in objcpp_commands:
      result = subprocess.run(command.split() + ["--dry-run"], capture_output=True, text=True, check=True)

      objcpp_file = [item for item in command.split() if item.endswith(".mm")]
      assert(len(objcpp_file))
      file = objcpp_file[0]
      directory = output_dir
      command = result.stdout.replace("\n", "")
      json_array.append({
        "directory": directory,
        "command": command,
        "file": os.path.relpath(file, directory),
      })

    with open(json_file, "w") as json_file:
      json_file.write(json.dumps(json_array, indent=4))

  except subprocess.CalledProcessError as e:
    print(e.stderr)


def parse_cli_args():
  parser = argparse.ArgumentParser(prog="generate_compiler_commands")

  subparsers = parser.add_subparsers(dest="command")

  compile_command = subparsers.add_parser("generate")
  compile_command.set_defaults(command="generate")
  compile_command.add_argument("--out-dir")
  compile_command.add_argument("--target")
  compile_command.add_argument("--json")
  compile_command.set_defaults(func=generate)

  args = parser.parse_args()
  if hasattr(args, "func"):
    args.func(args)

cli_args = parse_cli_args()
