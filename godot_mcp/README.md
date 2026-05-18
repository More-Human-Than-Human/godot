# Godot MCP

Shared MCP server for Godot projects.

It provides:

- GDScript LSP diagnostics, hover, definitions, references, completion, and symbols.
- Static project introspection for scenes, scripts, classes, inputs, dependencies, and UIDs.
- Validation for `res://` references, scene ext_resources, GDExtension libraries, and UID sidecars.
- Headless Godot runner tools for scripts, scenes, and discovered tests.
- Optional live editor control through the `godot_mcp_bridge` Godot addon.

## Build

```sh
cd /Users/zelda/Agents/mcps/godot_mcp
cargo build --release
```

The binary is:

```text
/Users/zelda/Agents/mcps/godot_mcp/target/release/godot_mcp
```

## MCP config

Point your MCP client at the shared binary and set the project path per project.

```json
{
  "mcpServers": {
    "GodotMCP": {
      "command": "/Users/zelda/Agents/mcps/godot_mcp/target/release/godot_mcp",
      "env": {
        "GODOT_PROJECT_PATH": "/absolute/path/to/project.godot-folder",
        "GODOT_BINARY": "/Applications/Godot.app/Contents/MacOS/Godot"
      }
    }
  }
}
```

## Environment

| Variable | Default | Purpose |
| --- | --- | --- |
| `GODOT_PROJECT_PATH` | `{cwd}/godot` | Folder containing `project.godot` |
| `GODOT_BINARY` | auto-detected | Godot executable for headless runs |
| `GODOT_LSP_HOST` | `127.0.0.1` | Godot LSP host |
| `GODOT_LSP_PORT` | `6005` | Godot LSP port |
| `GODOT_EDITOR_RPC_HOST` | `127.0.0.1` | Editor bridge host |
| `GODOT_EDITOR_RPC_PORT` | `6007` | Editor bridge port |
| `GODOT_STRESS_SCENE` | unset | Optional benchmark scene for `run_stress` |
| `GODOT_EXTENSION_HEALTH_SCRIPT` | unset | Optional script for `query_rust_extension` |

## Editor Bridge

Live editor tools require the Godot addon from:

```text
/Users/zelda/Agents/mcps/godot_mcp/godot_addons/godot_mcp_bridge
```

Copy that folder into another Godot project as:

```text
addons/godot_mcp_bridge
```

Then enable it in Godot under Project -> Project Settings -> Plugins. The bridge listens on port `6007` by default.

## Notes

The shared MCP is project-neutral. DesertGame-specific helpers are now opt-in through environment variables:

- Set `GODOT_STRESS_SCENE=res://scenes/StressWorld.tscn` to use the benchmark helper.
- Set `GODOT_EXTENSION_HEALTH_SCRIPT=res://scripts/tests/RustExtensionHealthCheck.gd` to use the extension health check helper.
