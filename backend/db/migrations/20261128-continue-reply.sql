ALTER TABLE messages
  ADD COLUMN generation_id VARCHAR(80) CHARACTER SET ascii COLLATE ascii_bin NULL,
  ADD COLUMN model_provider TEXT NULL,
  ADD COLUMN model_name TEXT NULL,
  ADD COLUMN reasoning_effort VARCHAR(8) CHARACTER SET ascii NULL;

ALTER TABLE turn_requests
  ADD COLUMN replay_content_bytes INT UNSIGNED NULL,
  ADD COLUMN replay_reasoning_bytes INT UNSIGNED NULL,
  ADD COLUMN replay_finish_reason VARCHAR(16) NULL;
