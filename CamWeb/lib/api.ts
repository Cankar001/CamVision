import { endSession } from "./auth";

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

/** A request to the API of the backend (same origin, the login cookie goes along). Fails with an ApiError, which has the text of the backend. */
export async function api<T>(method: "GET" | "POST" | "PUT" | "DELETE", url: string, body?: unknown): Promise<T> {
  let response: Response;
  try {
    response = await fetch(url, {
      method,
      credentials: "same-origin",
      headers: body === undefined ? undefined : { "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError("The backend is not reachable.", 0);
  }

  let data: unknown = null;
  try {
    data = await response.json();
  } catch {
    // an answer without a body
  }

  if (response.status === 401 && url !== "/api/login") {
    // The login ended: the guard of the pages sends the user to the login screen.
    endSession();
    throw new ApiError("Your login ended. Please log in again.", 401);
  }

  if (!response.ok) {
    const message = data && typeof data === "object" && "error" in data ? String((data as { error: unknown }).error) : `The backend answered ${response.status}.`;
    throw new ApiError(message, response.status);
  }

  return data as T;
}
