// Where cfbsim-server is: the address the viewer and the Builder both use,
// remembered in the browser's localStorage.

const SERVER_KEY = "cfbsim.server";
export const DEFAULT_SERVER = "ws://localhost:8765";

export function serverUrl(): string {
  try {
    return localStorage.getItem(SERVER_KEY) || DEFAULT_SERVER;
  } catch {
    return DEFAULT_SERVER;
  }
}

export function rememberServer(url: string): void {
  try {
    localStorage.setItem(SERVER_KEY, url);
  } catch {
    // Not remembered.
  }
}
