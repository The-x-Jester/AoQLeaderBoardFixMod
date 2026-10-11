const BOARDS = Object.freeze({
  "titan-kills": { mode: "max", description: "Most Titans killed" },
  "titan-wins": { mode: "max", description: "Most Titan encounters won" },
  "fastest-run-ms": { mode: "min", description: "Fastest run in milliseconds" },
});

// Best-effort per-isolate throttle. This is not a global/distributed limit.
const rateBuckets = new Map();

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
      "access-control-allow-methods": "GET, POST, OPTIONS",
      "access-control-allow-headers": "Content-Type",
      "x-content-type-options": "nosniff",
    },
  });
}

function cleanText(value, maxLength) {
  if (typeof value !== "string") return "";
  return value.trim().slice(0, maxLength);
}

function getBoard(boardId) {
  return typeof boardId === "string" && Object.hasOwn(BOARDS, boardId)
    ? BOARDS[boardId]
    : null;
}

function parseScore(value) {
  if (value === null || value === undefined || value === "") return null;
  const score = typeof value === "number" ? value : Number(value);
  if (!Number.isSafeInteger(score) || score < 0 || score > 2_000_000_000) return null;
  return score;
}

function rateLimited(request) {
  const now = Date.now();
  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  let b = rateBuckets.get(ip);
  if (!b || now - b.start >= 60_000) {
    b = { start: now, count: 0 };
    rateBuckets.set(ip, b);
  }
  b.count++;
  if (rateBuckets.size > 5000) {
    for (const [key, val] of rateBuckets) {
      if (now - val.start >= 60_000) rateBuckets.delete(key);
    }
  }
  return b.count > 90;
}

async function saveScore(db, input) {
  const boardId = cleanText(String(input.boardId ?? ""), 64);
  const config = getBoard(boardId);
  if (!config) {
    return { status: 400, body: { error: "unknown_board", knownBoards: Object.keys(BOARDS) } };
  }

  const playerId = cleanText(String(input.playerId ?? ""), 128);
  const playerName = cleanText(String(input.name ?? "Unknown"), 48) || "Unknown";
  const regiment = cleanText(String(input.regiment ?? ""), 64);
  const regimentProvided = input.regimentProvided === true;
  const score = parseScore(input.score);
  if (!playerId || /^(null|undefined)$/i.test(playerId)) {
    return { status: 400, body: { error: "player_id_required" } };
  }
  if (score === null) {
    return { status: 400, body: { error: "score_must_be_a_nonnegative_integer" } };
  }

  let scoreUpdate;
  if (config.mode === "max") scoreUpdate = "score = MAX(leaderboard_scores.score, excluded.score)";
  else if (config.mode === "min") scoreUpdate = "score = MIN(leaderboard_scores.score, excluded.score)";
  else scoreUpdate = "score = excluded.score";

  await db.prepare(`
    INSERT INTO leaderboard_scores
      (board_id, player_id, player_name, score, regiment, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
    ON CONFLICT(board_id, player_id) DO UPDATE SET
      player_name = excluded.player_name,
      regiment = CASE WHEN ? = 1 THEN excluded.regiment ELSE leaderboard_scores.regiment END,
      ${scoreUpdate},
      updated_at = CURRENT_TIMESTAMP
  `).bind(boardId, playerId, playerName, score, regiment, regimentProvided ? 1 : 0).run();

  const saved = await db.prepare(`
    SELECT board_id AS boardId, player_id AS playerId,
           player_name AS name, score, regiment, updated_at AS updatedAt
    FROM leaderboard_scores WHERE board_id = ? AND player_id = ?
  `).bind(boardId, playerId).first();

  // Monthly kills are derived from changes in the lifetime total supplied by
  // Scoring.UpdateKills -> LeaderboardMongoDB.UpdatePlayer. The first score
  // seen after enabling this feature is a baseline only, never a retroactive
  // monthly award. One atomic upsert computes the increase, so retries of the
  // same total do not award the same kills twice.
  if (boardId === "titan-kills") {
    const state = await db.prepare(`
      INSERT INTO leaderboard_player_state
        (board_id, player_id, player_name, last_total, last_increment, regiment, updated_at)
      VALUES (?, ?, ?, ?, 0, ?, CURRENT_TIMESTAMP)
      ON CONFLICT(board_id, player_id) DO UPDATE SET
        player_name = excluded.player_name,
        regiment = CASE WHEN ? = 1 THEN excluded.regiment ELSE leaderboard_player_state.regiment END,
        last_increment = CASE
          WHEN excluded.last_total > leaderboard_player_state.last_total
          THEN excluded.last_total - leaderboard_player_state.last_total
          ELSE 0
        END,
        last_total = excluded.last_total,
        updated_at = CURRENT_TIMESTAMP
      RETURNING last_increment AS delta, player_name AS name, regiment
    `).bind(boardId, playerId, playerName, score, regiment,
            regimentProvided ? 1 : 0).first();

    const delta = Number(state?.delta || 0);
    if (delta > 0) {
      const monthKey = new Date().toISOString().slice(0, 7); // UTC calendar month
      await db.prepare(`
        INSERT INTO leaderboard_monthly_scores
          (month_key, board_id, player_id, player_name, score, regiment, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)
        ON CONFLICT(month_key, board_id, player_id, regiment) DO UPDATE SET
          player_name = excluded.player_name,
          score = leaderboard_monthly_scores.score + excluded.score,
          updated_at = CURRENT_TIMESTAMP
      `).bind(monthKey, boardId, playerId, state.name || playerName,
              delta, state.regiment || "").run();
    }
  }

  return { status: 200, body: { ok: true, ...saved, mode: config.mode } };
}

async function handle(request, env) {
  const url = new URL(request.url);
  const path = url.pathname.replace(/\/+$/, "") || "/";

  if (request.method === "OPTIONS") return json({ ok: true });
  if (rateLimited(request)) return json({ error: "rate_limit_exceeded" }, 429);

  if (request.method === "GET" && path === "/") {
    return json({ service: "AoQ Leaderboard API", ok: true, health: "/health", boards: "/api/v1/leaderboards" });
  }
  if (request.method === "GET" && path === "/health") {
    return json({ ok: true, databaseConfigured: Boolean(env.DB) });
  }
  if (request.method === "GET" && path === "/api/v1/leaderboards") {
    return json({ boards: Object.entries(BOARDS).map(([id, info]) => ({ id, ...info })) });
  }

  // Compatibility route for Attack on Quest's old UpdatePlayer?id=&name=&total= URL.
  if (request.method === "GET" && path === "/UpdatePlayer") {
    try {
      const result = await saveScore(env.DB, {
        boardId: url.searchParams.get("board") || "titan-kills",
        playerId: url.searchParams.get("id"),
        name: url.searchParams.get("name"),
        score: url.searchParams.get("total"),
        regiment: url.searchParams.get("regiment") || "",
        regimentProvided: url.searchParams.has("regiment"),
      });
      return json(result.body, result.status);
    } catch (error) {
      console.error("Legacy UpdatePlayer failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  const writeMatch = path.match(/^\/api\/v1\/leaderboards\/([^/]+)\/scores$/);
  if (request.method === "POST" && writeMatch) {
    let body;
    try { body = await request.json(); }
    catch { return json({ error: "invalid_json" }, 400); }
    try {
      const result = await saveScore(env.DB, {
        boardId: decodeURIComponent(writeMatch[1]),
        playerId: body?.playerId,
        name: body?.name,
        score: body?.score,
        regiment: body?.regiment ?? "",
        regimentProvided: body != null && Object.hasOwn(body, "regiment"),
      });
      return json(result.body, result.status);
    } catch (error) {
      console.error("Score submission failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  // Wall-menu compatibility route. AoQ's Regiments.DisplayTopTen callback
  // expects the response to be a bare JSON array where each entry has
  // { "name": ..., "kills": ... }; it does not expect the normal {entries: []}
  // envelope returned by the public API endpoints.
  const wallMatch = path.match(/^\/api\/v1\/leaderboards\/([^/]+)\/wall$/);
  if (request.method === "GET" && wallMatch) {
    const boardId = decodeURIComponent(wallMatch[1]);
    if (!getBoard(boardId)) return json({ error: "unknown_board", knownBoards: Object.keys(BOARDS) }, 404);
    if (boardId !== "titan-kills") return json({ error: "wall_compatibility_supported_only_for_titan_kills" }, 404);

    const rawLimit = Number.parseInt(url.searchParams.get("limit") || "10", 10);
    const limit = Number.isFinite(rawLimit) ? Math.max(1, Math.min(10, rawLimit)) : 10;
    const period = (url.searchParams.get("period") || "lifetime").toLowerCase();
    const regimentProvided = url.searchParams.has("regiment");
    const regimentInput = cleanText(url.searchParams.get("regiment") || "", 64);
    const normRegiment = regimentInput.toLowerCase().replace(/[^a-z0-9]/g, "");

    // Normalize common label/data variants (e.g. "Scout", "Scouts",
    // "Military Police", or "MilitaryPolice") before matching existing rows.
    let regimentAliases = null;
    if (regimentProvided) {
      if (["scout", "scouts", "scoutregiment", "scoutinglegion"].includes(normRegiment)) {
        regimentAliases = ["scout", "scouts", "scoutregiment", "scoutinglegion"];
      } else if (["militarypolice", "militarypolices", "militarypolicecorps", "militarypoliceregiment", "mp"].includes(normRegiment)) {
        regimentAliases = ["militarypolice", "militarypolices", "militarypolicecorps", "militarypoliceregiment", "mp"];
      } else if (["garrison", "garrisons", "garrisonregiment"].includes(normRegiment)) {
        regimentAliases = ["garrison", "garrisons", "garrisonregiment"];
      } else {
        regimentAliases = [normRegiment];
      }
    }

    try {
      let resultRows;
      if (period === "monthly") {
        const monthArg = url.searchParams.get("month");
        const monthKey = monthArg && /^\d{4}-(0[1-9]|1[0-2])$/.test(monthArg)
          ? monthArg : new Date().toISOString().slice(0, 7);
        const normalizedSql = "LOWER(REPLACE(REPLACE(REPLACE(regiment, ' ', ''), '-', ''), '_', ''))";
        const filterSql = regimentProvided
          ? ` AND ${normalizedSql} IN (${regimentAliases.map(() => "?").join(",")})`
          : "";
        const query = await env.DB.prepare(`
          SELECT MAX(player_name) AS name, SUM(score) AS kills
          FROM leaderboard_monthly_scores
          WHERE board_id = ? AND month_key = ?${filterSql}
          GROUP BY player_id
          ORDER BY kills DESC, name ASC
          LIMIT ?
        `).bind(boardId, monthKey, ...(regimentAliases || []), limit).all();
        resultRows = query.results || [];
      } else if (period === "lifetime") {
        const normalizedSql = "LOWER(REPLACE(REPLACE(REPLACE(regiment, ' ', ''), '-', ''), '_', ''))";
        const filterSql = regimentProvided
          ? ` AND ${normalizedSql} IN (${regimentAliases.map(() => "?").join(",")})`
          : "";
        const query = await env.DB.prepare(`
          SELECT player_name AS name, score AS kills
          FROM leaderboard_scores
          WHERE board_id = ?${filterSql}
          ORDER BY score DESC, updated_at ASC
          LIMIT ?
        `).bind(boardId, ...(regimentAliases || []), limit).all();
        resultRows = query.results || [];
      } else {
        return json({ error: "period_must_be_lifetime_or_monthly" }, 400);
      }

      // Always return a bare array. DisplayTopTen fills missing rows with
      // "NAME" / 0 itself when fewer than ten players qualify.
      return json((resultRows || []).map((row) => ({
        name: row.name || "NAME",
        kills: Number(row.kills || 0),
      })));
    } catch (error) {
      console.error("Wall-menu leaderboard read failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  // Monthly individual leaderboard, optionally limited to one regiment.
  const monthlyMatch = path.match(/^\/api\/v1\/leaderboards\/([^/]+)\/monthly$/);
  if (request.method === "GET" && monthlyMatch) {
    const boardId = decodeURIComponent(monthlyMatch[1]);
    if (!getBoard(boardId)) return json({ error: "unknown_board", knownBoards: Object.keys(BOARDS) }, 404);
    if (boardId !== "titan-kills") return json({ error: "monthly_supported_only_for_titan_kills" }, 404);
    const rawLimit = Number.parseInt(url.searchParams.get("limit") || "10", 10);
    const limit = Number.isFinite(rawLimit) ? Math.max(1, Math.min(100, rawLimit)) : 10;
    const monthArg = url.searchParams.get("month");
    const monthKey = monthArg && /^\d{4}-(0[1-9]|1[0-2])$/.test(monthArg)
      ? monthArg : new Date().toISOString().slice(0, 7);
    const regimentProvided = url.searchParams.has("regiment");
    const regiment = cleanText(url.searchParams.get("regiment") || "", 64);
    try {
      const query = regimentProvided
        ? await env.DB.prepare(`
            SELECT player_id AS playerId, player_name AS name, score, regiment, updated_at AS updatedAt
            FROM leaderboard_monthly_scores
            WHERE board_id = ? AND month_key = ? AND regiment = ?
            ORDER BY score DESC, updated_at ASC LIMIT ?
          `).bind(boardId, monthKey, regiment, limit).all()
        : await env.DB.prepare(`
            SELECT player_id AS playerId, MAX(player_name) AS name, SUM(score) AS score,
                   '' AS regiment, MAX(updated_at) AS updatedAt
            FROM leaderboard_monthly_scores
            WHERE board_id = ? AND month_key = ?
            GROUP BY player_id
            ORDER BY score DESC, updatedAt ASC LIMIT ?
          `).bind(boardId, monthKey, limit).all();
      return json({ boardId, month: monthKey, mode: "monthly-count", entries: (query.results || []).map((row, index) => ({ rank: index + 1, ...row })) });
    } catch (error) {
      console.error("Monthly leaderboard read failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  // Team competition: total monthly kills grouped by regiment.
  const regimentMonthlyMatch = path.match(/^\/api\/v1\/leaderboards\/([^/]+)\/monthly\/regiments$/);
  if (request.method === "GET" && regimentMonthlyMatch) {
    const boardId = decodeURIComponent(regimentMonthlyMatch[1]);
    if (!getBoard(boardId)) return json({ error: "unknown_board", knownBoards: Object.keys(BOARDS) }, 404);
    if (boardId !== "titan-kills") return json({ error: "monthly_supported_only_for_titan_kills" }, 404);
    const monthArg = url.searchParams.get("month");
    const monthKey = monthArg && /^\d{4}-(0[1-9]|1[0-2])$/.test(monthArg)
      ? monthArg : new Date().toISOString().slice(0, 7);
    const rawLimit = Number.parseInt(url.searchParams.get("limit") || "10", 10);
    const limit = Number.isFinite(rawLimit) ? Math.max(1, Math.min(100, rawLimit)) : 10;
    try {
      const query = await env.DB.prepare(`
        SELECT regiment, SUM(score) AS kills, COUNT(*) AS players
        FROM leaderboard_monthly_scores
        WHERE board_id = ? AND month_key = ? AND regiment <> ''
        GROUP BY regiment
        ORDER BY kills DESC, regiment ASC LIMIT ?
      `).bind(boardId, monthKey, limit).all();
      return json({ boardId, month: monthKey, mode: "monthly-regiment-count", entries: (query.results || []).map((row, index) => ({ rank: index + 1, ...row })) });
    } catch (error) {
      console.error("Monthly regiment leaderboard read failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  const readMatch = path.match(/^\/api\/v1\/leaderboards\/([^/]+)$/);
  if (request.method === "GET" && readMatch) {
    const boardId = decodeURIComponent(readMatch[1]);
    const config = getBoard(boardId);
    if (!config) return json({ error: "unknown_board", knownBoards: Object.keys(BOARDS) }, 404);
    const rawLimit = Number.parseInt(url.searchParams.get("limit") || "10", 10);
    const limit = Number.isFinite(rawLimit) ? Math.max(1, Math.min(100, rawLimit)) : 10;
    const direction = config.mode === "min" ? "ASC" : "DESC";
    try {
      const regimentFilterProvided = url.searchParams.has("regiment");
      const regimentFilter = cleanText(url.searchParams.get("regiment") || "", 64);
      const query = regimentFilterProvided
        ? await env.DB.prepare(`
            SELECT player_id AS playerId, player_name AS name,
                   score, regiment, updated_at AS updatedAt
            FROM leaderboard_scores
            WHERE board_id = ? AND regiment = ?
            ORDER BY score ${direction}, updated_at ASC
            LIMIT ?
          `).bind(boardId, regimentFilter, limit).all()
        : await env.DB.prepare(`
            SELECT player_id AS playerId, player_name AS name,
                   score, regiment, updated_at AS updatedAt
            FROM leaderboard_scores
            WHERE board_id = ?
            ORDER BY score ${direction}, updated_at ASC
            LIMIT ?
          `).bind(boardId, limit).all();
      const rows = query.results || [];
      return json({
        boardId,
        mode: config.mode,
        entries: rows.map((row, index) => ({ rank: index + 1, ...row })),
      });
    } catch (error) {
      console.error("Leaderboard read failed", error);
      return json({ error: "internal_error" }, 500);
    }
  }

  return json({ error: "not_found" }, 404);
}

export default {
  async fetch(request, env) {
    try {
      if (!env.DB) return json({ error: "database_binding_missing" }, 500);
      return await handle(request, env);
    } catch (error) {
      console.error("Unhandled API error", error);
      return json({ error: "internal_error" }, 500);
    }
  },
};
