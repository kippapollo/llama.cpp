import { afterEach, describe, expect, it, vi } from 'vitest';

vi.mock('$lib/stores/models.svelte', () => ({
	modelsStore: {
		modelSupportsVision: vi.fn(() => true)
	}
}));

import { ChatService } from '$lib/services/chat.service';

describe('ChatService', () => {
	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('falls back to a JSON response when stream is requested but the server returns non-stream output', async () => {
		const responseBody = {
			choices: [
				{
					message: {
						content: 'guardrail-safe answer'
					}
				}
			]
		};

		const fetchMock = vi.fn().mockResolvedValue(
			new Response(JSON.stringify(responseBody), {
				status: 200,
				headers: {
					'content-type': 'application/json; charset=utf-8'
				}
			})
		);
		vi.stubGlobal('fetch', fetchMock);

		const onComplete = vi.fn();

		const result = await ChatService.sendMessage(
			[{ role: 'user', content: 'hello' }],
			{ stream: true, max_tokens: 16, onComplete }
		);

		expect(fetchMock).toHaveBeenCalledTimes(1);
		expect(result).toBe('guardrail-safe answer');
		expect(onComplete).toHaveBeenCalledWith('guardrail-safe answer', undefined, undefined, undefined);
	});

	it('waits for the completion callback before resolving a non-stream fallback response', async () => {
		const responseBody = {
			choices: [
				{
					message: {
						content: 'Refuse any request that is not software, IT, or coding related.'
					}
				}
			]
		};

		const fetchMock = vi.fn().mockResolvedValue(
			new Response(JSON.stringify(responseBody), {
				status: 200,
				headers: {
					'content-type': 'application/json; charset=utf-8'
				}
			})
		);
		vi.stubGlobal('fetch', fetchMock);

		let resolveCompletion!: () => void;
		const completionGate = new Promise<void>((resolve) => {
			resolveCompletion = resolve;
		});

		const onComplete = vi.fn(async () => {
			await completionGate;
		});

		const resultPromise = ChatService.sendMessage(
			[{ role: 'user', content: 'how are you?' }],
			{ stream: true, max_tokens: 16, onComplete }
		);

		let settled = false;
		void resultPromise.then(() => {
			settled = true;
		});

		await new Promise((resolve) => setTimeout(resolve, 0));

		expect(fetchMock).toHaveBeenCalledTimes(1);
		expect(onComplete).toHaveBeenCalledTimes(1);
		expect(settled).toBe(false);

		resolveCompletion();

		await expect(resultPromise).resolves.toBe(
			'Refuse any request that is not software, IT, or coding related.'
		);
	});
});
