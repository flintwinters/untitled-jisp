#!/usr/bin/env python3
import subprocess
import sys
from rich.console import Console
import typer

app = typer.Typer()
console = Console()

# Compiler and flags
CC = "gcc"
CFLAGS = ["-Wall", "-Wextra", "-Werror", "-std=c11", "-I.", "-g", "-O0"]
LDFLAGS = []

# Source files and target executable
SRCS = ["jisp.c", "yyjson.c"]
TARGET = "jisp"

@app.command()
def build():
    """Build and run the executable."""
    try:
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
            # Run the executable
            console.print(f"[cyan]Running {TARGET}...[/cyan]\n")
            result = subprocess.run([
                "valgrind", 
                "--errors-for-leak-kinds=all",
                "--error-exitcode=1",
                "--leak-check=full",
                "--show-leak-kinds=all",
                "./" + TARGET
            ], capture_output=True, text=True)
            if result.stdout:
                console.print(result.stdout, highlight=True)
            if result.stderr:
                console.print(result.stderr, highlight=True)
    except subprocess.CalledProcessError:
        console.print("[red]Build process encountered an error.[/red]")

if __name__ == "__main__":
    app()
