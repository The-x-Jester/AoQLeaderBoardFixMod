CREATE TABLE IF NOT EXISTS leaderboard_scores (
  board_id TEXT NOT NULL,
  player_id TEXT NOT NULL,
  player_name TEXT NOT NULL,
  score INTEGER NOT NULL CHECK (score >= 0),
  regiment TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (board_id, player_id)
);

CREATE INDEX IF NOT EXISTS idx_leaderboard_scores_rank
  ON leaderboard_scores (board_id, score DESC, updated_at ASC);

CREATE INDEX IF NOT EXISTS idx_leaderboard_scores_updated
  ON leaderboard_scores (board_id, updated_at DESC);
