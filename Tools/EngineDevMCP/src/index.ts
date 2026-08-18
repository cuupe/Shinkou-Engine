import { McpServer } from "@modelcontextprotocol/server";
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import * as z from "zod/v4";

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { promisify } from "node:util";
import { readFile, stat } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";

const execFileAsync = promisify(execFile);

const ROOT = process.env.ENGINE_ROOT ?? process.cwd();

const BUILD_SCRIPT = process.env.ENGINE_BUILD_SCRIPT;
const BUILD_DIR =
  process.env.ENGINE_BUILD_DIR ?? path.join(ROOT, "out/build/editor");

const EDITOR_EXE =
  process.env.ENGINE_EDITOR_EXE ??
  path.join(ROOT, "Bin", "Editor.exe");

const LOG_FILE =
  process.env.ENGINE_EDITOR_LOG ??
  path.join(ROOT, "Saved", "Logs", "Engine.log");

let editorProcess: ChildProcess | null = null;

function createServer(): McpServer {
  const server = new McpServer(
    {
      name: "engine-dev",
      version: "0.1.0",
    },
    {
      instructions: `
This MCP controls the local game-engine development environment.

Use engine_build before claiming changes compile.
Use engine_get_log when Editor behavior is abnormal.
Use engine_web_request to probe the engine's local HTTP/WebSocket gateway.
Do not launch multiple Editor instances unless necessary.
Do not claim runtime behavior was verified unless the corresponding tool was actually called.
      `.trim(),
    },
  );

  // --------------------------------------------------
  // build
  // --------------------------------------------------

  server.registerTool(
    "engine_build",
    {
      description: "Build the game engine Editor target.",
      inputSchema: z.object({
        config: z
          .enum(["Debug", "Development", "Release"])
          .default("Development"),
      }),
    },
    async ({ config }) => {
      try {
        let result;

        if (BUILD_SCRIPT && existsSync(BUILD_SCRIPT)) {
          if (process.platform === "win32") {
            result = await execFileAsync("cmd.exe", ["/d", "/s", "/c", `"${BUILD_SCRIPT}" ${config}`], {
              cwd: ROOT,
              maxBuffer: 32 * 1024 * 1024,
            });
          } else {
            result = await execFileAsync(BUILD_SCRIPT, [config], {
              cwd: ROOT,
              maxBuffer: 32 * 1024 * 1024,
            });
          }
        } else if (process.platform === "win32") {
          result = await execFileAsync(
            "cmake",
            ["--build", BUILD_DIR, "--config", config, "--target", "shinkou_engine"],
            {
              cwd: ROOT,
              maxBuffer: 32 * 1024 * 1024,
            },
          );
        } else {
          result = await execFileAsync(
            "cmake",
            ["--build", BUILD_DIR, "--config", config, "--target", "shinkou_engine"],
            {
              cwd: ROOT,
              maxBuffer: 32 * 1024 * 1024,
            },
          );
        }

        return {
          content: [
            {
              type: "text",
              text:
                `Build succeeded.\n\n` +
                `stdout:\n${result.stdout}\n\n` +
                `stderr:\n${result.stderr}`,
            },
          ],
        };
      } catch (error: any) {
        return {
          isError: true,
          content: [
            {
              type: "text",
              text:
                `Build failed.\n` +
                `${error?.stdout ?? ""}\n` +
                `${error?.stderr ?? ""}\n` +
                `${error?.message ?? error}`,
            },
          ],
        };
      }
    },
  );

  // --------------------------------------------------
  // launch editor
  // --------------------------------------------------

  server.registerTool(
    "engine_launch_editor",
    {
      description: "Launch the game engine Editor.",
      inputSchema: z.object({
        args: z.array(z.string()).default([]),
      }),
    },
    async ({ args }) => {
      if (editorProcess && !editorProcess.killed) {
        return {
          content: [
            {
              type: "text",
              text: `Editor is already running, pid=${editorProcess.pid}`,
            },
          ],
        };
      }

      editorProcess = spawn(
        EDITOR_EXE,
        args,
        {
          cwd: ROOT,
          stdio: "ignore",
        },
      );

      return {
        content: [
          {
            type: "text",
            text: `Editor launched, pid=${editorProcess.pid}`,
          },
        ],
      };
    },
  );

  // --------------------------------------------------
  // log
  // --------------------------------------------------

  server.registerTool(
    "engine_get_log",
    {
      description: "Read the latest lines from the Editor log.",
      inputSchema: z.object({
        lines: z.number().int().min(1).max(2000).default(200),
        contains: z.string().optional(),
        level: z.enum(["trace", "debug", "info", "warn", "error", "critical"]).optional(),
      }),
    },
    async ({ lines, contains, level }) => {
      try {
        const text = await readFile(LOG_FILE, "utf8");

        const filtered = text
          .split(/\r?\n/)
          .filter((line) => !contains || line.includes(contains))
          .filter((line) => !level || line.toLowerCase().includes(`] [${level}]`));
        const result = filtered
          .slice(-lines)
          .join("\n");

        return {
          content: [
            {
              type: "text",
              text: result,
            },
          ],
        };
      } catch (error: any) {
        return {
          isError: true,
          content: [
            {
              type: "text",
              text: `Failed to read log: ${error.message}`,
            },
          ],
        };
      }
    },
  );

  server.registerTool(
    "engine_log_stats",
    {
      description: "Read size and modification metadata for the ShinkouEngine log.",
      inputSchema: z.object({}),
    },
    async () => {
      try {
        const metadata = await stat(LOG_FILE);
        return {
          content: [{
            type: "text",
            text: JSON.stringify({
              path: LOG_FILE,
              sizeBytes: metadata.size,
              modifiedAt: metadata.mtime.toISOString(),
            }, null, 2),
          }],
        };
      } catch (error: any) {
        return {
          isError: true,
          content: [{ type: "text", text: `Failed to stat log: ${error.message}` }],
        };
      }
    },
  );

  // --------------------------------------------------
  // web gateway
  // --------------------------------------------------

  server.registerTool(
    "engine_web_request",
    {
      description: "Send an HTTP request to a running ShinkouEngine web gateway.",
      inputSchema: z.object({
        url: z.string().url(),
        method: z.enum(["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"]).default("GET"),
        headers: z.record(z.string(), z.string()).default({}),
        body: z.string().optional(),
        timeoutMs: z.number().int().min(100).max(30000).default(5000),
      }),
    },
    async ({ url, method, headers, body, timeoutMs }) => {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), timeoutMs);
      try {
        const response = await fetch(url, {
          method,
          headers,
          body: method === "GET" || method === "DELETE" ? undefined : body,
          signal: controller.signal,
        });
        const responseBody = await response.text();
        return {
          isError: !response.ok,
          content: [{
            type: "text",
            text: JSON.stringify({
              status: response.status,
              statusText: response.statusText,
              headers: Object.fromEntries(response.headers.entries()),
              body: responseBody,
            }, null, 2),
          }],
        };
      } catch (error: any) {
        return {
          isError: true,
          content: [{ type: "text", text: `HTTP request failed: ${error?.message ?? error}` }],
        };
      } finally {
        clearTimeout(timeout);
      }
    },
  );

  // --------------------------------------------------
  // shutdown
  // --------------------------------------------------

  server.registerTool(
    "engine_shutdown",
    {
      description: "Stop the Editor instance launched by this MCP server.",
      inputSchema: z.object({}),
    },
    async () => {
      if (!editorProcess) {
        return {
          content: [
            {
              type: "text",
              text: "No managed Editor process is running.",
            },
          ],
        };
      }

      editorProcess.kill();
      editorProcess = null;

      return {
        content: [
          {
            type: "text",
            text: "Editor stopped.",
          },
        ],
      };
    },
  );

  return server;
}

void serveStdio(createServer);

// 注意：stdio MCP 绝对不要 console.log()
console.error("EngineDevMCP running.");
