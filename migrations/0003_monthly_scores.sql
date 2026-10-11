CREATE TABLE IF NOT EXISTS leaderboard_player_state (
  board_id TEXT NOT NULL,
  player_id TEXT NOT NULL,
  player_name TEXT NOT NULL,
  last_total INTEGER NOT NULL CHECK (last_total >= 0),
  last_increment INTEGER NOT NULL DEFAULT 0 CHECK (last_increment >= 0),
  regiment TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (board_id, player_id)
);

CREATE TABLE IF NOT EXISTS leaderboard_monthly_scores (
  month_key TEXT NOT NULL,
  board_id TEXT NOT NULL,
  player_id TEXT NOT NULL,
  player_name TEXT NOT NULL,
  score INTEGER NOT NULL DEFAULT 0 CHECK (score >= 0),
  regiment TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (month_key, board_id, player_id, regiment)
);

CREATE INDEX IF NOT EXISTS idx_monthly_scores_rank
  ON leaderboard_monthly_scores (board_id, month_key, score DESC, updated_at ASC);

CREATE INDEX IF NOT EXISTS idx_monthly_scores_regiment
  ON leaderboard_monthly_scores (board_id, month_key, regiment, score DESC);
