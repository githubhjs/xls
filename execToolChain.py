#!/home/josh_huang/.local/bin/python3
import argparse
import subprocess
import os

# Global variable for the binary directory
GOOGLEXLS_BIN="/home/josh_huang/devel/googlexls/xls-patchelf"

def run_command(command, input_file, output_file, check=True):
    """Runs a command with input file as an argument and checks for errors."""
    command_with_input = command + [input_file]
    try:
        with open(output_file, 'w') as outfile:
            subprocess.run(command_with_input, stdout=outfile, stderr=subprocess.PIPE, check=check, text=True)
        print(f"Command executed successfully: {command_with_input}")
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {command_with_input}")
        print(f"Return code: {e.returncode}")
        print(f"Stdout: {e.stdout}")
        print(f"Stderr: {e.stderr}")
        exit(1)
    except FileNotFoundError:
        print(f"Error: Input file '{input_file}' not found.")
        exit(1)


def main():
    parser = argparse.ArgumentParser(description="Build flow script.")
    parser.add_argument("input_file", help="Input filename (with extension)")
    parser.add_argument("--top", help="Value for --top argument", default="add")
    parser.add_argument("--pipeline_stages", type=int, help="Value for --pipeline_stages", default=1)
    parser.add_argument("--delay_model", help="Value for --delay_model", default="unit")

    args = parser.parse_args()

    input_file = args.input_file
    base_filename = os.path.splitext(input_file)[0]

    # Construct full paths to binaries using the global variable
    interpreter_main = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", os.path.join(GOOGLEXLS_BIN, "interpreter_main")]
    ir_converter_main = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", os.path.join(GOOGLEXLS_BIN, "ir_converter_main"), f"--top={args.top}"]
    opt_main = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", os.path.join(GOOGLEXLS_BIN, "opt_main")]
    codegen_main = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", os.path.join(GOOGLEXLS_BIN, "codegen_main"), f"--pipeline_stages={args.pipeline_stages}", f"--delay_model={args.delay_model}"]

    run_command(interpreter_main, input_file, "/dev/null")
    run_command(ir_converter_main, f"{base_filename}.x", f"{base_filename}.ir")
    run_command(opt_main, f"{base_filename}.ir", f"{base_filename}.opt.ir")
    run_command(codegen_main, f"{base_filename}.opt.ir", f"{base_filename}.v")

    print("Success! Output files created:")
    print(f"{base_filename}.x")
    print(f"{base_filename}.ir")
    print(f"{base_filename}.opt.ir")
    print(f"{base_filename}.v")


if __name__ == "__main__":
    main()
#
#
#def run_command(command, input_file, output_file, check=True):
#    """Runs a command with input file as an argument and checks for errors."""
#    command_with_input = command + [input_file]  # Add input file as argument
#    try:
#        with open(output_file, 'w') as outfile:
#            subprocess.run(command_with_input, stdout=outfile, stderr=subprocess.PIPE, check=check, text=True)
#        print(f"Command executed successfully: {command_with_input}")
#    except subprocess.CalledProcessError as e:
#        print(f"Error executing command: {command_with_input}")
#        print(f"Return code: {e.returncode}")
#        print(f"Stdout: {e.stdout}")
#        print(f"Stderr: {e.stderr}")
#        exit(1)
#    except FileNotFoundError:
#        print(f"Error: Input file '{input_file}' not found.")
#        exit(1)
#
#
#def main():
#    parser = argparse.ArgumentParser(description="Build flow script.")
#    parser.add_argument("input_file", help="Input filename (with extension)")  # Now takes full filename
#    parser.add_argument("--top", help="Value for --top argument", default="add")
#    parser.add_argument("--pipeline_stages", type=int, help="Value for --pipeline_stages", default=1)
#    parser.add_argument("--delay_model", help="Value for --delay_model", default="unit")
#
#    args = parser.parse_args()
#
#    input_file = args.input_file # full filename
#    base_filename = os.path.splitext(input_file)[0]  # Extract filename without extension
#
#    interpreter_main =  [ "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "${GOOGLEXLS_BIN}/interpreter_main"]
#    ir_converter_main = [ "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "${GOOGLEXLS_BIN}/ir_converter_main", "--top", args.top]
#    opt_main =          [ "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "${GOOGLEXLS_BIN}/opt_main"]
#    codegen_main =      [ "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "${GOOGLEXLS_BIN}/codegen_main", f"--pipeline_stages={args.pipeline_stages}", f"--delay_model={args.delay_model}"]
#
#    run_command(interpreter_main, input_file, f"{base_filename}_add.x")  # Corrected
#    run_command(ir_converter_main, f"{base_filename}_add.x", f"{base_filename}_add.ir")
#    run_command(opt_main, f"{base_filename}_add.ir", f"{base_filename}_add.opt.ir")
#    run_command(codegen_main, f"{base_filename}_add.opt.ir", f"{base_filename}_add.v")
#
#    print("Success! Output files created:")
#    print(f"{base_filename}_add.x")
#    print(f"{base_filename}_add.ir")
#    print(f"{base_filename}_add.opt.ir")
#    print(f"{base_filename}_add.v")
#
#
#if __name__ == "__main__":
#    main()
#
##import argparse
##import subprocess
##import os
##
##def run_command(command, input_file, output_file, check=True):
##    """Runs a command and checks for errors."""
##    try:
##        with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
##           #subprocess.run(command+input_file, stdin=infile, stdout=outfile, stderr=subprocess.PIPE, check=check, text=True)  # text=True handles strings
##            subprocess.run(command+input_file,               stdout=outfile, stderr=subprocess.PIPE, check=check, text=True)  # text=True handles strings
##        print(f"Command executed successfully: {command}")
##    except subprocess.CalledProcessError as e:
##        print(f"Error executing command: {command}")
##        print(f"Return code: {e.returncode}")
##        print(f"Stdout: {e.stdout}")
##        print(f"Stderr: {e.stderr}")
##        exit(1)
##    except FileNotFoundError:
##        print(f"Error: Input file '{input_file}' not found.")
##        exit(1)
##
##
##def main():
##    parser = argparse.ArgumentParser(description="Build flow script.")
##    parser.add_argument("input_file", help="Input filename (without extension)")
##    parser.add_argument("--top", help="Value for --top argument", default="add")  # Default to "add"
##    parser.add_argument("--pipeline_stages", type=int, help="Value for --pipeline_stages", default=1) # Default to 1
##    parser.add_argument("--delay_model", help="Value for --delay_model", default="unit") # Default to unit
##
##    args = parser.parse_args()
##
##    filename = args.input_file
##    base_filename = os.path.splitext(filename)[0] # Extract filename without extension
##
##    interpreter_main  = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "/home/josh_huang/devel/googlexls/xls-patchelf/interpreter_main"]
##    ir_converter_main = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "/home/josh_huang/devel/googlexls/xls-patchelf/ir_converter_main", "--top", args.top]
##    opt_main          = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "/home/josh_huang/devel/googlexls/xls-patchelf/opt_main"]
##    codegen_main      = ["/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64/ld-linux-x86-64.so.2", "--library-path", "/home/josh_huang/.local/opt/glibc-centos9stream/usr/lib64", "/home/josh_huang/devel/googlexls/xls-patchelf/codegen_main", f"--pipeline_stages     = {args.pipeline_stages}", f"--delay_model = {args.delay_model}"]
##
##
##    run_command(interpreter_main  , f"{base_filename}.x"          , f"{base_filename}_add.x")
##    run_command(ir_converter_main , f"{base_filename}_add.x"      , f"{base_filename}_add.ir")
##    run_command(opt_main          , f"{base_filename}_add.ir"     , f"{base_filename}_add.opt.ir")
##    run_command(codegen_main      , f"{base_filename}_add.opt.ir" , f"{base_filename}_add.v")
##
##    print("Success! Output files created:")
##    print(f"{base_filename}_add.x")
##    print(f"{base_filename}_add.ir")
##    print(f"{base_filename}_add.opt.ir")
##    print(f"{base_filename}_add.v")
##
##
##if __name__ == "__main__":
##    main()
