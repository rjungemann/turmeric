import { test, expect } from '@playwright/test';

// /ci — the CI suite-timings dashboard.
//
// These hit the live worker route, which proxies the `ci-metrics` branch on
// GitHub. That is deliberate: the proxy is the part most likely to break
// silently (a renamed branch, a year rollover), and a fixture would hide it.

test.describe('CI metrics dashboard', () => {
  test('worker proxies the timings NDJSON', async ({ request }) => {
    const res = await request.get('/api/ci-timings');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('ndjson');
    expect(res.headers()['x-timings-year']).toMatch(/^\d{4}$/);

    const lines = (await res.text()).trim().split('\n');
    expect(lines.length).toBeGreaterThan(100);

    const row = JSON.parse(lines[0]);
    for (const key of ['suite', 'duration_ms', 'ts', 'os', 'cc', 'status', 'sha']) {
      expect(row).toHaveProperty(key);
    }
  });

  test('page renders every section without console errors', async ({ page }) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

    await page.goto('/ci');

    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#ci-state')).toBeHidden();

    // Shared chrome still renders on this page.
    await expect(page.locator('site-nav nav')).toBeVisible();
    await expect(page.locator('site-footer a[href="/ci"]')).toHaveCount(1);

    // Four stat tiles, and a provenance line naming the latest commit.
    await expect(page.locator('#ci-tiles .ci-tile')).toHaveCount(4);
    await expect(page.locator('#ci-provenance .mono').first()).toHaveText(/^[0-9a-f]{7}$/);

    // The chart drew real geometry, and the legend accounts for every series.
    const lines = page.locator('#ci-chart .ci-series-line');
    await expect(lines.first()).toBeVisible();
    const seriesCount = await lines.count();
    expect(seriesCount).toBeGreaterThan(0);
    expect(seriesCount).toBeLessThanOrEqual(5);
    await expect(page.locator('#ci-legend .ci-legend-item')).toHaveCount(seriesCount);

    await expect(page.locator('#ci-sparks .ci-spark').first()).toBeVisible();
    await expect(page.locator('#ci-table tbody tr').first()).toBeVisible();
    await expect(page.locator('#ci-skips').first()).not.toBeEmpty();

    expect(errors).toEqual([]);
  });

  test('the environment lock offers each build environment and switching redraws', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const env = page.locator('#ci-env');
    const options = await env.locator('option').allTextContents();
    expect(options.length).toBeGreaterThan(1);
    // Labels name the OS and compiler, since those are what make runs
    // incomparable across environments.
    expect(options.join(' ')).toMatch(/macOS|Linux/);

    const before = await page.locator('#ci-table tbody').innerHTML();
    await env.selectOption({ index: 1 });
    await expect(page.locator('#ci-table tbody')).not.toHaveText('');
    expect(await page.locator('#ci-table tbody').innerHTML()).not.toBe(before);
    await expect(page).toHaveURL(/env=/);
  });

  test('removing a series does not recolor the survivors', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const swatches = page.locator('#ci-legend .ci-legend-item .swatch');
    await expect(swatches).toHaveCount(5);
    const secondColor = await swatches.nth(1).evaluate(
      (el) => getComputedStyle(el).backgroundColor,
    );

    // Drop the first series; the one that was second keeps its own color.
    await page.locator('#ci-legend .ci-legend-item').first().click();
    await expect(swatches).toHaveCount(4);
    const nowFirst = await swatches.first().evaluate(
      (el) => getComputedStyle(el).backgroundColor,
    );
    expect(nowFirst).toBe(secondColor);
  });

  test('scale toggle and status filter are reflected in the URL', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('.ci-seg-btn[data-scale="log"]').click();
    await expect(page).toHaveURL(/scale=log/);
    await expect(page.locator('#ci-chart .ci-series-line').first()).toBeVisible();

    await page.locator('.ci-chip[data-status="skip"]').click();
    await expect(page.locator('.ci-chip[data-status="skip"]')).toHaveAttribute('aria-pressed', 'false');
    await expect(page).toHaveURL(/status=/);
  });

  test('hovering the plot opens a tooltip', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    await page.locator('#ci-hit').hover();
    const tip = page.locator('#ci-tooltip');
    await expect(tip).toBeVisible();
    await expect(tip.locator('.ci-tooltip-head .mono')).toHaveText(/^[0-9a-f]{7}$/);
    await expect(tip.locator('.ci-tooltip-row').first()).toBeVisible();
  });

  test('sparkline filter narrows the grid and clicking charts a suite', async ({ page }) => {
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const sparks = page.locator('#ci-sparks .ci-spark');
    const total = await sparks.count();
    expect(total).toBeGreaterThan(20);

    await page.locator('#ci-spark-search').fill('jit');
    await expect(sparks).not.toHaveCount(total);

    const target = sparks.first();
    const name = await target.getAttribute('data-suite');
    await target.click();
    await expect(page.locator(`#ci-legend .ci-legend-item[data-suite="${name}"]`)).toHaveCount(1);
  });

  test('layout does not overflow horizontally at 480px', async ({ page }) => {
    await page.setViewportSize({ width: 480, height: 900 });
    await page.goto('/ci');
    await expect(page.locator('#ci-body')).toBeVisible({ timeout: 20_000 });

    const overflow = await page.evaluate(
      () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
    );
    expect(overflow).toBeLessThanOrEqual(1);
  });
});
