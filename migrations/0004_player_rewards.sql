-- AoQ permanent regiment cape rewards. Apply once to the EXISTING D1 database.
CREATE TABLE IF NOT EXISTS aoq_player_profiles (
  player_id TEXT PRIMARY KEY,
  player_name TEXT NOT NULL DEFAULT 'Unknown',
  unlock_flags TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS aoq_reward_months (
  month_key TEXT PRIMARY KEY,
  finalized_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS aoq_reward_awards (
  month_key TEXT NOT NULL,
  regiment TEXT NOT NULL,
  player_id TEXT NOT NULL,
  player_name TEXT NOT NULL,
  rank INTEGER NOT NULL,
  score INTEGER NOT NULL,
  flag TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (month_key, regiment, player_id)
);
CREATE INDEX IF NOT EXISTS idx_aoq_reward_awards_player ON aoq_reward_awards(player_id);
