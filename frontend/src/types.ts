export type AuthMode = 'login' | 'register' | 'forgot' | 'reset';
export type AccountView = 'home' | 'username' | 'password' | 'email' | 'email-edit' | 'devices' | 'logout-all' | 'logout-device';

export interface User {
  id: string;
  username: string;
  email: string | null;
}

export interface DeviceSession {
  id: string;
  user_agent: string;
  created_at: number;
  last_active_at: number;
  expires_at: number;
  current: boolean;
}

export type ReasoningEffort = 'high' | 'max';

export interface Model {
  provider: string;
  model: string;
  reasoning_efforts?: ReasoningEffort[];
}

export type ChatRequest = {
  conversation_id: string;
  request_id: string;
  stream: boolean;
} & (
  | (Pick<Model, 'provider' | 'model'> & { reasoning_effort?: ReasoningEffort } & (
    | { content: string; regenerate_message_id?: never }
    | { regenerate_message_id: string; content?: never }
  ))
  | { continue_message_id: string; continue_from: string }
);

export interface Message {
  // 本地消息在收到 meta 事件前尚未分配 ID 和持久化状态。
  id?: string;
  parent_id?: string | null;
  role: 'user' | 'assistant';
  state?: 'generating' | 'completed' | 'interrupted' | 'failed';
  content: string;
  reasoning?: string;
  finish_reason?: string;
  error?: string;
  interrupted?: boolean;
  truncated?: boolean;
  continue_from?: string;
  versions?: {
    index: number;
    total: number;
    previous_id: string | null;
    next_id: string | null;
  };
}

export interface ConversationSummary {
  id: string;
  title: string;
  current_message_id: string | null;
  active_message_id: string | null;
  created_at: number;
  updated_at: number;
}

export interface Conversation extends ConversationSummary {
  messages: Message[];
  loaded: boolean;
  provider?: string;
  model?: string;
  reasoningEffort?: ReasoningEffort;
}

export interface HistoryCursor {
  before_updated_at: string;
  before_id: string;
}

export interface PendingReply {
  controller: AbortController;
  text: string;
  reasoning: string;
  regenerate: boolean;
  continueId?: string;
  stopped: boolean;
  finished: boolean;
  assistantId?: string;
}

export interface PendingSend {
  content: string;
  raw: string;
  requestId: string;
  parentId: string | null;
  model: Model;
  reasoningEffort?: ReasoningEffort;
  error: string;
  rejected?: boolean;
}

export interface PendingRegenerate {
  questionId: string;
  previousAssistantId: string | undefined;
  requestId: string;
  model: Model;
  reasoningEffort?: ReasoningEffort;
  error: string;
}

export interface PendingContinue {
  answerId: string;
  revision: string;
  requestId: string;
  error: string;
}

export interface ReasoningView {
  details: HTMLDetailsElement;
  title: HTMLSpanElement;
  content: HTMLDivElement;
  text: string;
}

export interface GetResponses {
  '/api/auth/me': { user: User };
  [path: `/api/auth/sessions?${string}`]: { items: DeviceSession[]; next_cursor: string };
  '/api/models': { models: Model[] };
  '/api/conversations': { items: ConversationSummary[]; next: HistoryCursor | null };
  [path: `/api/conversations?${string}`]: GetResponses['/api/conversations'];
  [path: `/api/conversation?${string}`]: { conversation: ConversationSummary; messages: Message[] };
}

type Ok = { ok: boolean };
type EmptyBody = Record<string, never>;
export interface Registration {
  user: User;
  email_delivery: 'sent' | 'failed' | 'not_requested';
}

export interface PostContracts {
  '/api/auth/register': { body: { username: string; password: string; email?: string }; response: Registration };
  '/api/auth/login': { body: { identifier: string; password: string }; response: { user: User } };
  '/api/auth/logout': { body: EmptyBody; response: Ok };
  '/api/auth/logout/all': { body: EmptyBody; response: Ok };
  '/api/auth/sessions/revoke': { body: { session_id: string }; response: Ok };
  '/api/auth/username': { body: { username: string }; response: { user: User } };
  '/api/auth/password': { body: { old_password: string; new_password: string }; response: Ok };
  '/api/auth/password/verify': { body: { password: string }; response: Ok & { expires_in: number } };
  '/api/auth/password/forgot': { body: { email: string }; response: { message: string } };
  '/api/auth/password/reset': { body: { email: string; code: string; new_password: string }; response: Ok };
  '/api/auth/email/code': { body: { email: string }; response: Ok };
  '/api/auth/email/bind': { body: { email: string; code: string }; response: { user: User } };
  '/api/auth/email/unbind': { body: EmptyBody; response: { user: User } };
  '/api/conversations': { body: { title: string }; response: { conversation: ConversationSummary } };
  '/api/conversation/rename': { body: { conversation_id: string; title: string }; response: Ok };
  '/api/conversation/delete': { body: { conversation_id: string }; response: Ok };
  '/api/conversation/select': { body: { conversation_id: string; message_id: string }; response: { conversation: ConversationSummary; messages: Message[] } };
}
