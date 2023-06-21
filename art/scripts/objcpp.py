import argparse
import subprocess

def compile(args, unknown_args):
    try:
        if args.dry_run:
            print(" ".join([args.driver] + unknown_args))
            return
        result = subprocess.run([args.driver] + unknown_args, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(e.stderr)


def link(args, unknown_args):
    try:
        result = subprocess.run([args.driver] + unknown_args, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(e.stderr)


def parse_cli_args():
    parser = argparse.ArgumentParser(prog="objcpp")

    subparsers = parser.add_subparsers(dest="command")

    compile_command = subparsers.add_parser("compile")
    compile_command.add_argument("--dry-run", action="store_true")
    compile_command.add_argument("--driver")

    compile_command.set_defaults(func=compile)

    link_command = subparsers.add_parser("link")
    link_command.add_argument("--driver")
    link_command.add_argument("--args")
    link_command.set_defaults(func=link)

    args, unknown_args = parser.parse_known_args()
    if hasattr(args, "func"):
        args.func(args, unknown_args)

cli_args = parse_cli_args()
