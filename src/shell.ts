/**
 * Shell command execution helpers.
 */
import { execFile, spawn as nodeSpawn } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

/**
 * Execute a shell command and return stdout.
 * Commands are run through /bin/sh -c for pipeline/glob support.
 */
export async function exec(command: string): Promise<string> {
  const { stdout } = await execFileAsync("/bin/sh", ["-c", command], {
    maxBuffer: 50 * 1024 * 1024, // 50 MB for base64 screenshots
    timeout: 30_000,
  });
  return stdout;
}

/**
 * Spawn a long-running process. Returns the ChildProcess.
 */
export function spawn(command: string, args: string[]) {
  return nodeSpawn(command, args, { stdio: "ignore", detached: true });
}
