# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains a **self-contained DevContainer installer** for running Claude Code in a secure, Podman-compatible containerized environment. The project implements a unique build system that generates a single-file installer (`install.sh`) with all configuration files embedded as heredocs.

**Key concept**: The installer is built once and can be distributed as a single bash script that contains everything needed to set up the DevContainer configuration in any Git repository.

## Project Architecture

### Build System Architecture

The project uses a **two-stage distribution model**:

1. **Source files** (this repository):
   - `devcontainer.json` - DevContainer configuration with security hardening
   - `Dockerfile` - Container image definition
   - `generate-claude-config.sh` - Runtime authentication configuration script
   - `CLAUDE.md` - Documentation embedded in installer

2. **build.sh** - Build script that:
   - Reads all source files
   - Embeds them as heredocs in a single bash script
   - Adds extraction and installation logic
   - Produces self-contained `install.sh`

3. **install.sh** (generated artifact):
   - Single distributable file (~60KB)
   - Contains all configuration files embedded
   - Can be run in any Git repository
   - Extracts files to `.devcontainer/` directory

### Authentication Flow Architecture

Simple environment variable authentication using `ANTHROPIC_AUTH_TOKEN`:

```
Host System
  ↓ initializeCommand (runs before container creation)
  └─→ Capture ANTHROPIC_* environment variables from host
      ├─→ ANTHROPIC_AUTH_TOKEN → .devcontainer/.claude-auth-token (required)
      ├─→ ANTHROPIC_BASE_URL → .devcontainer/.claude-base-url (optional)
      └─→ ANTHROPIC_CUSTOM_HEADERS → .devcontainer/.claude-custom-headers (optional)

Container Creation
  ↓ postCreateCommand (runs inside container after creation)
  ├─→ Install Claude Code via npm
  └─→ Run generate-claude-config.sh
      ├─→ Read credential files from .devcontainer/
      ├─→ Generate ~/.claude.json (minimal config)
      └─→ Generate ~/.claude/settings.json with env vars:
          ├─→ ANTHROPIC_AUTH_TOKEN (required)
          ├─→ ANTHROPIC_BASE_URL (if set)
          └─→ ANTHROPIC_CUSTOM_HEADERS (if set)

Result: Claude Code authenticated without manual login
```

**Required environment variable**:
- `ANTHROPIC_AUTH_TOKEN` - Must be set in host environment before starting container

**Optional environment variables** (captured from host if set):
- `ANTHROPIC_BASE_URL` - Custom API endpoint
- `ANTHROPIC_CUSTOM_HEADERS` - Additional HTTP headers for API requests

**Critical implementation details**:
- `initializeCommand` runs on HOST before container exists
- `postCreateCommand` runs INSIDE container after it's created
- All credential files are gitignored and never committed
- Container will fail to configure if `ANTHROPIC_AUTH_TOKEN` is not set

### Security Architecture

Defense-in-depth container hardening via `runArgs` in devcontainer.json. This configuration balances developer productivity with strong isolation.

**Security model**: Multi-layered isolation where even gaining root inside the container cannot affect the host system.

1. **User Namespace Isolation** (`--userns=keep-id`):
   - Container runs in isolated user namespace with rootless Podman
   - Inside container: `vscode` user can use `sudo` to become namespace-root for package installation
   - On host: Container processes map to regular user UID—cannot become real root or access other users' files
   - **Critical property**: Sudo gives "root" inside namespace ONLY; cannot affect host system, modify host files outside workspace, or escalate to host root

2. **Capability Minimization** (`--cap-drop=ALL` then selective add):
   - Drops all Linux capabilities by default (removes ~30+ dangerous capabilities)
   - Only grants 6 essential capabilities for development:
     - **SETUID**: Required for sudo to change UID within namespace
     - **SETGID**: Required for sudo to change GID within namespace
     - **AUDIT_WRITE**: Allows authentication systems (PAM, sudo) to write audit logs
     - **CHOWN**: Allows package managers (apt, npm, pip) to change file ownership
     - **NET_BIND_SERVICE**: Allows binding to privileged ports (80, 443) without root
     - **NET_RAW**: Enables ICMP for network debugging (`ping`, `traceroute`)
   - All capabilities scoped to container namespace—cannot affect host system
   - Reduces attack surface by ~95% compared to default container

3. **Network Isolation** (`--network=slirp4netns`):
   - User-mode networking (no root privileges required)
   - Container cannot access host services (127.0.0.1 on host is unreachable)
   - Internet access via HTTP/HTTPS for package downloads
   - Prevents lateral movement to host services

4. **Privilege Escalation Prevention** (`--security-opt=no-new-privileges`):
   - Prevents privilege escalation via setuid binaries or file capabilities
   - Blocks exploiting setuid-root binaries even if found
   - Sudo relies on CAP_SETUID capability (allowed), not setuid binary bit (blocked)

5. **Resource Limits**:
   - CPU: `--cpus=8` (prevents CPU exhaustion)
   - Memory: `--memory=8g` (prevents memory exhaustion)
   - Swap: `--memory-swap=56g` (8GB RAM + 48GB swap, prevents OOM kills)
   - Not preallocated: Swap only uses disk when RAM is full

### File Ownership Model

The project root serves dual purposes:

1. **Source repository**: DevContainer configuration files for this project
2. **Installer generator**: Build system that creates distributable installer

When `build.sh` runs, it reads files from project root and embeds them in `install.sh`, which users run in *their* repositories to install the DevContainer configuration.

## Common Commands

### Build the Installer

```bash
# Generate install.sh with all files embedded
./build.sh
```

**What it does**:
- Reads all source files from project root
- Generates git commit hash and build date
- Creates heredocs for each file with EOF delimiters
- Produces self-contained install.sh (~60KB)
- Makes installer executable (chmod +x)

**Files embedded** (defined in build.sh:31-36):
1. devcontainer.json
2. Dockerfile
3. generate-claude-config.sh
4. CLAUDE.md (this file)

### Test the Installer Locally

```bash
# Create test repository
mkdir /tmp/test-repo && cd /tmp/test-repo
git init

# Run installer
bash /path/to/claude-container/install.sh
```

**Expected behavior**:
- Checks if .devcontainer/ already exists (errors if present)
- Shows interactive prompt with ESC to cancel
- Creates .devcontainer/ directory
- Extracts all embedded files
- Makes generate-claude-config.sh executable
- Adds credential files to .gitignore

### Run Claude Code in Unsupervised Mode

After the container is running, start Claude Code in fully autonomous mode:

```bash
# Inside the container
claude --dangerously-skip-permissions
```

**What this does**:
- Bypasses all permission prompts for tool usage
- Enables fully autonomous operation
- Allows Claude Code to execute commands without confirmation

**Security considerations**:
- ⚠️ Only use in sandboxed/isolated environments
- The container already provides isolation (capabilities dropped, network isolated, resource limited)
- This flag grants unrestricted access within the container's security boundaries
- Appropriate for CI/CD, automated workflows, or development in isolated containers

### Modify Configuration Files

When editing configuration files, you must rebuild the installer:

```bash
# 1. Edit source file
vim devcontainer.json

# 2. Rebuild installer
./build.sh

# 3. Commit both source and generated installer
git add devcontainer.json install.sh
git commit -m "Update devcontainer configuration"
```

**Important**: install.sh is a build artifact but is tracked in git because it's the primary distribution method. Both source files and generated installer must stay in sync.

### Adjust Resource Limits

Edit `devcontainer.json` runArgs array:

```jsonc
"runArgs": [
  // ... other flags ...
  "--cpus=12",         // Default: 8 (80% of 10-core host)
  "--memory=16g",      // Default: 8g minimum
  "--memory-swap=64g"  // Default: 56g (8g RAM + 48g swap)
]
```

**Default configuration:**
- CPU: 8 cores (~80% of 10-core system)
- RAM: 8GB minimum
- Swap: 48GB maximum (not preallocated)
- Total VM: 56GB

**Calculate for your host:**
```bash
# CPU: Use 80% of host cores
# macOS: sysctl -n hw.ncpu
# Linux: nproc
# Example: 16 cores × 0.8 = 12-13 cores

# RAM: At least 8GB, more for heavy builds
# Example: 32GB host → use 16GB or 24GB for container

# Swap: Keep 48GB or scale proportionally
# Example: 16GB RAM + 48GB swap = 64GB total
```

**Swap behavior:**
- `--memory-swap` = `--memory`: No swap (RAM only)
- `--memory-swap` > `--memory`: Enables swap (difference is swap size)
- Swap is not preallocated: Only uses disk when RAM is full

Then rebuild: `./build.sh`

## Dockerfile Architecture

### Base Image

Uses Microsoft's official DevContainer base image (`mcr.microsoft.com/devcontainers/base:ubuntu-22.04`):
- Pre-configured non-root `vscode` user (UID 1000)
- Common dev tools pre-installed (git, curl, wget, build-essential)
- Ubuntu 22.04 LTS (support until 2027)

### Installed Tools and Environment Setup

**Core tools** (always installed):
- Build tools: build-essential, cmake, ninja-build, pkg-config
- Python: python3, pip, venv
- Node.js: Latest LTS via NodeSource repository
- pkgx: User-space package manager for ad-hoc tool installation
- Git: Pre-configured with safe.directory for /workspace

**Optional development environments** (commented out, uncomment to enable):
- **Java**: OpenJDK 11, 17, 21 + Maven + Gradle (multi-architecture: AMD64/ARM64)
- **Rust**: rustup with cargo (stable/beta/nightly versions)
- **C++**: LLVM 18 with Clang, LLDB, LLD
- **Python**: Enhanced with conda, virtualenv, dev tools
- **Clojure**: Official Clojure CLI tools

### Multi-Architecture Support

The Dockerfile uses `$(dpkg --print-architecture)` for automatic architecture detection:
- Works on both AMD64 (Intel) and ARM64 (Apple Silicon)
- Java version selection commands use architecture-aware paths
- Example: `/usr/lib/jvm/java-17-openjdk-$(dpkg --print-architecture)/bin/java`

**Java architecture notes**:
- OpenJDK 11, 17, 21: Available on both AMD64 and ARM64
- OpenJDK 8: Removed from default configuration (legacy, AMD64-only)
- Use pkgx for other Java versions: `pkgx install openjdk.org@17`

### VSCode Server Directory Structure

VSCode DevContainers requires specific directory structure (created in Dockerfile):

```
/vscode/
└── vscode-server/
    └── extensionsCache/        # Shared extensions cache (777 permissions)

/home/vscode/
└── .vscode-server/
    └── extensionsCache/        # Per-user extensions cache (755 permissions)
```

**Why both locations**:
- `/vscode`: Can be volume-mounted to share server binaries across multiple containers
- `/home/vscode/.vscode-server`: Per-user data, isolated to this container
- VSCode syncs extensions between both caches during startup

**Critical implementation** (Dockerfile lines 320-336):
```dockerfile
RUN mkdir -p /vscode/vscode-server/extensionsCache \
             /home/vscode/.vscode-server/extensionsCache && \
    chown -R vscode:vscode /home/vscode/.vscode-server && \
    chmod -R 755 /home/vscode/.vscode-server && \
    chmod -R 777 /vscode
```

**Why this matters**: If `extensionsCache` subdirectories don't exist, VSCode startup fails with "can't cd to /home/vscode/.vscode-server/extensionsCache" error.

### Java Certificate Store Fix

Java installations include a fix for ca-certificates-java setup issues:

```dockerfile
&& /var/lib/dpkg/info/ca-certificates-java.postinst configure
```

This ensures the Java certificate store is properly initialized, preventing "No such file or directory" errors for `/etc/ssl/certs/java/cacerts` during OpenJDK installation.

## Key Implementation Details

### Why Heredoc Embedding?

The build system uses heredocs instead of base64 encoding or downloading files because:
- Human-readable installer (can inspect what will be installed)
- No dependencies (no base64, curl, wget required)
- Preserves exact file content including comments
- Single bash script is easily distributable via curl/wget

### Dynamic Configuration Generation

Authentication configuration is generated dynamically by `generate-claude-config.sh` using `jq -n`:
1. `~/.claude/settings.json` with `env.ANTHROPIC_AUTH_TOKEN`
2. `~/.claude.json` with minimal config (onboarding complete, token suffix)

No template file is needed - the script creates the JSON structure directly using jq.

### File Naming Convention in build.sh

Heredoc EOF delimiters use file names with special characters replaced:
- `devcontainer.json` → `EOF_devcontainer_json`
- `generate-claude-config.sh` → `EOF_generate_claude_config_sh`

This is done by `${file//[.-]/_}` bash substitution (line 199).

### Why install.sh is Tracked in Git

Though install.sh is a generated artifact, it's tracked in version control because:
- It's the primary distribution method (users curl from GitHub)
- Allows users to download without cloning entire repo
- Build date and commit hash embedded for traceability
- Simplifies distribution via `curl -fsSL https://raw.githubusercontent.com/.../install.sh`

## Modifying the Build System

### Adding a New File to Embed

1. Create the file in project root
2. Add to `FILES` array in build.sh (line 31-36):
   ```bash
   FILES=(
       "devcontainer.json"
       "Dockerfile"
       "your-new-file.txt"  # Add here
   )
   ```
3. Update installer display in build.sh if needed (line 152-157)
4. Rebuild: `./build.sh`

### Changing Installer Behavior

Installer logic is in three sections of build.sh:

1. **Header** (line 78-93): Shebang, version info
2. **Main logic** (line 94-193): Installation flow, error checking
3. **Footer** (line 209-268): Post-install, .gitignore updates, next steps

Edit the heredoc strings in build.sh, then rebuild.

### Testing Changes

```bash
# 1. Make changes to source files or build.sh
# 2. Rebuild
./build.sh

# 3. Test in clean directory
cd /tmp && mkdir test-project && cd test-project
git init
bash /path/to/claude-container/install.sh

# 4. Verify extraction
ls -la .devcontainer/
cat .devcontainer/devcontainer.json

# 5. Test in VSCode (optional)
code .
# F1 → "Dev Containers: Reopen in Container"
```

## Troubleshooting the Build System

### "Missing source file" error

**Cause**: build.sh expects all files in `FILES` array to exist

**Solution**: Ensure all files listed in build.sh:31-36 are present in project root

### Container resources don't match configuration (macOS)

**Symptom**: Container shows 4 CPUs, 2GB RAM despite devcontainer.json specifying 8 CPUs, 8GB RAM

**Cause**: Podman runs in a VM on macOS. The VM's resource limits override container limits.

**Solution**:
```bash
# Check current VM resources
podman machine list

# If VM has insufficient resources:
podman machine stop
podman machine set --cpus 10 --memory 16384  # 10 CPUs, 16GB RAM
podman machine start

# Verify
podman machine list  # Should show updated resources

# Rebuild container in VSCode
# F1 → "Dev Containers: Rebuild Container"
```

**Critical**: The Podman VM must have MORE resources than the container needs:
- Container: 8 CPUs → VM: 10+ CPUs (20-25% overhead)
- Container: 8GB RAM → VM: 16GB RAM (room for VM overhead)

**Check container resources**:
```bash
# Inside container
nproc  # Should show 8 (if VM has 10+)
free -h  # Should show ~8GB (if VM has 16GB)
```

### Installer creates malformed files

**Cause**: Heredoc EOF delimiter collision (file contains `EOF_filename` string)

**Solution**: Change delimiter name in build.sh:199 to something unique

### Git commit hash shows "unknown"

**Cause**: Not running in a git repository, or git not in PATH

**Solution**: Run build.sh from within the git repository, ensure git is installed

## Platform Compatibility Notes

### Cross-Platform Authentication

The `initializeCommand` uses standard shell commands that work on macOS, Linux, and Windows (WSL):
- Environment variables are captured using `printf` and shell conditionals
- No platform-specific commands are used

**Requirement**: Set `ANTHROPIC_AUTH_TOKEN` in your environment before starting the container.

### Podman vs Docker

Configuration works with both:
- `--userns=keep-id` is Podman-specific (Docker ignores it gracefully)
- `--cpus` and `--memory` work on both Docker and Podman
- Network mode `slirp4netns` is optimal for rootless Podman

Users can set VSCode to use Podman via settings: `"dev.containers.dockerPath": "podman"`

## Documentation Locations

The project has documentation in multiple locations:

1. **README.md** (project root): User-facing documentation, installation instructions
2. **CLAUDE.md** (this file, project root): Architecture and development guidance
3. **.devcontainer/CLAUDE.md** (generated): Copy of this file, embedded in installer, appears in user's projects

When updating documentation, remember that CLAUDE.md gets embedded in the installer and distributed to users.

