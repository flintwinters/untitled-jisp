#!/usr/bin/env python3
import subprocess
import sys
import os # Required for file system checks
from rich.console import Console
import typer

app = typer.Typer()
console = Console()

# Compiler and flags
CC = "gcc"
CFLAGS = [
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-std=c11",
    "-I.",
    "-g",
    "-O0"
]
LDFLAGS = []

# Source files and target executable
SRCS = ["jisp.c", "yyjson.c"]
TARGET = "jisp"

def is_rebuild_needed():
    """Checks if the target file needs to be rebuilt based on timestamps."""
    # 1. Check if target exists
    if not os.path.exists(TARGET):
        console.print(f"[yellow]Target '{TARGET}' does not exist. Rebuild required.[/yellow]")
        return True

    # 2. Get target's modification time
    try:
        target_mtime = os.path.getmtime(TARGET)
    except OSError as e:
        console.print(f"[red]Error getting target file time: {e}[/red]")
        return True # Assume rebuild required on error

    # 3. Check source files' modification times
    for src in SRCS:
        try:
            src_mtime = os.path.getmtime(src)
            if src_mtime > target_mtime:
                console.print(f"[yellow]Source file '{src}' is newer than target. Rebuild required.[/yellow]")
                return True
        except OSError as e:
            console.print(f"[red]Warning: Source file '{src}' not found. Assuming rebuild is required.[/red]")
            # If a source file is missing, we should probably trigger a build failure or assume rebuild is needed
            return True

    # 4. If all sources are older or same age, no rebuild is needed
    return False


@app.command()
def build():
    """Build and run the executable, conditionally recompiling based on timestamps."""

    # Conditional Build Step
    if is_rebuild_needed():
        console.print(f"[cyan]Compiling {', '.join(SRCS)} into {TARGET}...[/cyan]")
        # Compile and link in one step
        result = subprocess.run([CC, *CFLAGS, *SRCS, "-o", TARGET, *LDFLAGS],
                                capture_output=True, text=True)
        if result.returncode != 0:
            console.print(f"[red]Build failed![/red]")
            if result.stdout:
                console.print(f"[yellow]{result.stdout}[/yellow]")
            if result.stderr:
                console.print(f"{result.stderr}")
            raise typer.Exit(1)
        else:
            console.print(f"[green]Build succeeded![/green]\n")
    else:
        console.print(f"[green]Target '{TARGET}' is up-to-date. Skipping compilation.[/green]\n")


    # Run the executable (runs regardless of whether compilation was skipped or performed)
    console.print(f"[cyan]Running {TARGET}...[/cyan]\n")
    result = subprocess.run([
        "valgrind", 
        "--errors-for-leak-kinds=all",
        "--error-exitcode=1",
        "--leak-check=full",
        "--show-leak-kinds=all",
        "./" + TARGET,
        "test.json"
    ], capture_output=True, text=True)
    if result.stdout:
        console.print(result.stdout, highlight=True)
    if result.stderr:
        console.print(result.stderr, highlight=True)

if __name__ == "__main__":
    app()