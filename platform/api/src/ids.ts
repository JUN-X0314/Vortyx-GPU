// Id syntax (Phase 11) — the single TypeScript definition of a syntactically
// valid platform id. Mirrors is_valid_id in src/platform/identity.hpp:
// 1..128 characters of [A-Za-z0-9._-]. Device ids, job ids and user-supplied
// resource references are all validated with this ONE rule; generated ids
// (UUID v4, lowercase) satisfy it by construction.

export const MAX_ID_LENGTH = 128;

const ID_PATTERN = /^[A-Za-z0-9._-]+$/;

export function isValidId(value: string): boolean {
  return value.length >= 1 && value.length <= MAX_ID_LENGTH && ID_PATTERN.test(value);
}
