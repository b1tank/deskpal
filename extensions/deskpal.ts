import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { spawn } from "node:child_process";
import { access } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

type JsonObject = Record<string, unknown>;

type McpTool = {
	name: string;
	description: string;
	inputSchema: JsonObject;
};

type McpContent =
	| { type: "text"; text: string }
	| { type: "image"; data: string; mimeType?: string };

type McpToolResult = {
	content?: McpContent[];
	isError?: boolean;
	[key: string]: unknown;
};

type PendingRequest = {
	resolve: (value: JsonObject) => void;
	reject: (error: Error) => void;
	timer: NodeJS.Timeout;
	removeAbort?: () => void;
};

const DEFAULT_TIMEOUT_MS = 120_000;
const MAX_RESPONSE_BYTES = 128 * 1024 * 1024;
const DEFAULT_BINARY = fileURLToPath(new URL("../build/deskpal", import.meta.url));

function enabled(name: string): boolean {
	return /^(1|true|yes)$/i.test(process.env[name] ?? "");
}

function toolLabel(name: string): string {
	return `Deskpal ${name.replaceAll("_", " ")}`;
}

function resultText(result: McpToolResult): string {
	return (result.content ?? [])
		.filter((item): item is Extract<McpContent, { type: "text" }> => item.type === "text")
		.map((item) => item.text)
		.join("\n");
}

class DeskpalBridge {
	private readonly child: ChildProcessWithoutNullStreams;
	private readonly pending = new Map<number, PendingRequest>();
	private stdoutBuffer = "";
	private stdoutBytes = 0;
	private nextId = 1;
	private stderrTail = "";
	private stopped = false;

	private constructor(child: ChildProcessWithoutNullStreams) {
		this.child = child;
		this.child.stdout.setEncoding("utf8");
		this.child.stderr.setEncoding("utf8");
		this.child.stdout.on("data", (chunk: string) => this.handleStdout(chunk));
		this.child.stderr.on("data", (chunk: string) => {
			this.stderrTail = (this.stderrTail + chunk).slice(-4096);
		});
		this.child.once("exit", (code, signal) => {
			this.stopped = true;
			const suffix = this.stderrTail.trim();
			const error = new Error(
				`Deskpal exited (${signal ? `signal ${signal}` : `code ${code ?? "unknown"}`})${suffix ? `: ${suffix}` : ""}`,
			);
			this.rejectAll(error);
		});
	}

	static async start(binary: string): Promise<DeskpalBridge> {
		await access(binary);
		const args: string[] = [];
		if (enabled("DESKPAL_PI_ALLOW_EXEC")) args.push("--allow-exec");
		if (enabled("DESKPAL_PI_ALLOW_FS")) args.push("--allow-fs");
		const bridge = new DeskpalBridge(spawn(binary, args, { stdio: ["pipe", "pipe", "pipe"] }));
		await bridge.request("initialize", {
			protocolVersion: "2024-11-05",
			capabilities: {},
			clientInfo: { name: "deskpal-pi", version: "0.1.0" },
		});
		bridge.notify("notifications/initialized", {});
		return bridge;
	}

	get running(): boolean {
		return !this.stopped && this.child.exitCode === null;
	}

	async listTools(): Promise<McpTool[]> {
		const response = await this.request("tools/list", {});
		return (response.tools ?? []) as McpTool[];
	}

	async callTool(name: string, args: JsonObject, signal?: AbortSignal): Promise<McpToolResult> {
		return (await this.request("tools/call", { name, arguments: args }, signal)) as McpToolResult;
	}

	async stop(): Promise<void> {
		if (this.stopped) return;
		this.stopped = true;
		this.child.stdin.end();
		await this.waitForExit(500);
		if (this.child.exitCode === null) {
			this.child.kill("SIGTERM");
			await this.waitForExit(500);
		}
		if (this.child.exitCode === null) {
			this.child.kill("SIGKILL");
			await this.waitForExit(500);
		}
		this.rejectAll(new Error("Deskpal session stopped"));
	}

	private waitForExit(timeoutMs: number): Promise<void> {
		if (this.child.exitCode !== null) return Promise.resolve();
		return new Promise((resolve) => {
			const timer = setTimeout(resolve, timeoutMs);
			this.child.once("exit", () => {
				clearTimeout(timer);
				resolve();
			});
		});
	}

	private notify(method: string, params: JsonObject): void {
		this.write({ jsonrpc: "2.0", method, params });
	}

	private request(method: string, params: JsonObject, signal?: AbortSignal): Promise<JsonObject> {
		if (!this.running) return Promise.reject(new Error("Deskpal is not running"));
		if (signal?.aborted) return Promise.reject(new Error("Deskpal request cancelled"));
		const id = this.nextId++;
		return new Promise((resolve, reject) => {
			const timer = setTimeout(() => {
				this.pending.delete(id);
				reject(new Error(`Deskpal request timed out: ${method}`));
			}, DEFAULT_TIMEOUT_MS);
			const pending: PendingRequest = { resolve, reject, timer };
			if (signal) {
				const abort = () => {
					this.pending.delete(id);
					clearTimeout(timer);
					this.child.kill("SIGTERM");
					reject(new Error("Deskpal request cancelled"));
				};
				signal.addEventListener("abort", abort, { once: true });
				pending.removeAbort = () => signal.removeEventListener("abort", abort);
			}
			this.pending.set(id, pending);
			this.write({ jsonrpc: "2.0", id, method, params });
		});
	}

	private write(message: JsonObject): void {
		this.child.stdin.write(`${JSON.stringify(message)}\n`);
	}

	private handleStdout(chunk: string): void {
		this.stdoutBuffer += chunk;
		this.stdoutBytes += Buffer.byteLength(chunk, "utf8");
		let newline: number;
		while ((newline = this.stdoutBuffer.indexOf("\n")) !== -1) {
			const line = this.stdoutBuffer.slice(0, newline);
			this.stdoutBytes -= Buffer.byteLength(
				this.stdoutBuffer.slice(0, newline + 1), "utf8");
			this.stdoutBuffer = this.stdoutBuffer.slice(newline + 1);
			this.handleLine(line);
		}
		if (this.stdoutBytes > MAX_RESPONSE_BYTES) {
			this.rejectAll(new Error("Deskpal response exceeded 128 MiB without a delimiter"));
			this.child.kill("SIGTERM");
		}
	}

	private handleLine(line: string): void {
		let response: JsonObject;
		try {
			response = JSON.parse(line) as JsonObject;
		} catch {
			this.rejectAll(new Error(`Deskpal emitted non-JSON stdout: ${line.slice(0, 200)}`));
			this.child.kill("SIGTERM");
			return;
		}
		if (typeof response.id !== "number") return;
		const pending = this.pending.get(response.id);
		if (!pending) return;
		this.pending.delete(response.id);
		clearTimeout(pending.timer);
		pending.removeAbort?.();
		const rpcError = response.error as { message?: string } | undefined;
		if (rpcError) pending.reject(new Error(rpcError.message ?? "Deskpal JSON-RPC error"));
		else pending.resolve((response.result ?? {}) as JsonObject);
	}

	private rejectAll(error: Error): void {
		for (const pending of this.pending.values()) {
			clearTimeout(pending.timer);
			pending.removeAbort?.();
			pending.reject(error);
		}
		this.pending.clear();
	}
}

export default function deskpalExtension(pi: ExtensionAPI) {
	const binary = process.env.DESKPAL_BINARY || DEFAULT_BINARY;
	const registered = new Set<string>();
	let bridge: DeskpalBridge | undefined;

	const ensureBridge = async (): Promise<DeskpalBridge> => {
		if (bridge?.running) return bridge;
		bridge = await DeskpalBridge.start(binary);
		return bridge;
	};

	const registerTools = async (): Promise<number> => {
		const server = await ensureBridge();
		const tools = await server.listTools();
		for (const tool of tools) {
			const piName = `deskpal_${tool.name}`;
			if (registered.has(piName)) continue;
			registered.add(piName);
			pi.registerTool({
				name: piName,
				label: toolLabel(tool.name),
				description: tool.description,
				parameters: tool.inputSchema as never,
				async execute(_toolCallId, params, signal) {
					const result = await (await ensureBridge()).callTool(tool.name, params as JsonObject, signal);
					if (result.isError) throw new Error(resultText(result) || `Deskpal ${tool.name} failed`);
					const content = (result.content ?? []).map((item) =>
						item.type === "image"
							? {
									type: "image" as const,
									data: item.data,
									mimeType: item.mimeType ?? "image/png",
								}
							: { type: "text" as const, text: item.text },
					);
					const metadata = Object.fromEntries(
						Object.entries(result).filter(([key]) => key !== "content" && key !== "isError"),
					);
					return { content, details: { ...metadata, deskpalTool: tool.name } };
				},
			});
		}
		return tools.length;
	};

	pi.on("session_start", async (_event, ctx) => {
		try {
			const count = await registerTools();
			ctx.ui.setStatus("deskpal", `deskpal: ${count} tools`);
		} catch (error) {
			ctx.ui.setStatus("deskpal", "deskpal: unavailable");
			ctx.ui.notify(error instanceof Error ? error.message : String(error), "warning");
		}
	});

	pi.on("session_shutdown", async () => {
		await bridge?.stop();
		bridge = undefined;
	});

	pi.registerCommand("deskpal-status", {
		description: "Show Deskpal Pi extension status",
		handler: async (_args, ctx) => {
			try {
				const count = await registerTools();
				ctx.ui.notify(`Deskpal is running with ${count} MCP tools registered.`, "info");
			} catch (error) {
				ctx.ui.notify(error instanceof Error ? error.message : String(error), "error");
			}
		},
	});
}
