CREATE TABLE IF NOT EXISTS users (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  username VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  password_hash VARCHAR(255) CHARACTER SET ascii NOT NULL,
  email VARCHAR(254) CHARACTER SET ascii COLLATE ascii_bin NULL,
  disabled BOOLEAN NOT NULL DEFAULT FALSE,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  UNIQUE KEY users_username (username),
  UNIQUE KEY users_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS conversations (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  title VARCHAR(256) NOT NULL,
  current_message_id BIGINT UNSIGNED NULL,
  active_message_id BIGINT UNSIGNED NULL,
  generation_deadline BIGINT NULL,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  KEY conversations_owner_page (user_id, updated_at DESC, id DESC),
  CONSTRAINT conversations_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS messages (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  conversation_id BIGINT UNSIGNED NOT NULL,
  parent_id BIGINT UNSIGNED NULL,
  role ENUM('user','assistant') NOT NULL,
  state ENUM('generating','completed','interrupted','failed') NOT NULL,
  content MEDIUMTEXT NOT NULL,
  reasoning MEDIUMTEXT NOT NULL,
  finish_reason VARCHAR(16) NOT NULL DEFAULT '',
  error VARCHAR(512) NOT NULL DEFAULT '',
  sequence BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  KEY messages_conversation (conversation_id, id),
  CONSTRAINT messages_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS turn_requests (
  conversation_id BIGINT UNSIGNED NOT NULL,
  request_id VARCHAR(80) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  request_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  user_message_id BIGINT UNSIGNED NOT NULL,
  assistant_message_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (conversation_id, request_id),
  CONSTRAINT requests_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
