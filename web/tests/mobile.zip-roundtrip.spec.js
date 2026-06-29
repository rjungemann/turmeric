import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiTabs);
    // Auto-accept the replace-confirm dialog
    page.on('dialog', (dialog) => dialog.accept());
}

test.describe('ZIP Roundtrip End-to-End (Phase 6)', () => {
    test('download project as ZIP and load it back via drag-and-drop', async ({ page }) => {
        await gotoTry(page);

        // 1. Create a 3-tab workspace with distinct content and order.
        // First tab (main.tur)
        await page.evaluate(() => window._turiEditor.setValue('(println "hello main")'));
        
        // Second tab (util.tur)
        const utilId = await page.evaluate(() => window._turiTabs.create({ name: 'util.tur' }).id);
        await page.evaluate(() => window._turiEditor.setValue('(defn square [x] (* x x))'));
        
        // Third tab (math.tur)
        const mathId = await page.evaluate(() => window._turiTabs.create({ name: 'math.tur' }).id);
        await page.evaluate(() => window._turiEditor.setValue('(defn cube [x] (* x (* x x)))'));

        // Activate the third tab (math.tur) as the active tab.
        await page.evaluate((id) => window._turiTabs.switch(id), mathId);
        
        // Wait for Monaco value & storage debounce to stabilize.
        await page.waitForTimeout(1000);

        // 2. Trigger download of the zip
        const [download] = await Promise.all([
            page.waitForEvent('download', { timeout: 15_000 }),
            (async () => {
                await page.locator('#more-btn').click();
                await page.locator('.more-item[data-cmd="download-btn"]').click();
            })(),
        ]);
        const downloadPath = await download.path();
        expect(downloadPath).toBeTruthy();
        const zipBytes = await readFile(downloadPath);

        // Verify the ZIP signature
        expect(zipBytes[0]).toBe(0x50);  // 'P'
        expect(zipBytes[1]).toBe(0x4b);  // 'K'
        expect(zipBytes[2]).toBe(0x03);
        expect(zipBytes[3]).toBe(0x04);

        // 3. Mutate the current workspace to have junk/mutated state
        await page.evaluate(() => {
            // Close all tabs except the first one
            const currentTabs = window._turiTabs.tabs();
            for (const t of currentTabs.slice(1)) {
                window._turiTabs.close(t.id);
            }
            window._turiEditor.setValue(';; mutated junk');
        });
        
        // Wait for a bit and verify that workspace is indeed mutated
        expect(await page.evaluate(() => window._turiTabs.tabs())).toHaveLength(1);
        expect(await page.evaluate(() => window._turiEditor.getValue())).toBe(';; mutated junk');

        // 4. Drag and drop the captured zip back onto the editor
        await page.evaluate(async (bytesBase64) => {
            const binStr = atob(bytesBase64);
            const bytes = new Uint8Array(binStr.length);
            for (let i = 0; i < binStr.length; i++) {
                bytes[i] = binStr.charCodeAt(i);
            }
            const file = new File([bytes], 'try-turmeric-roundtrip.zip', { type: 'application/zip' });
            const dataTransfer = new DataTransfer();
            dataTransfer.items.add(file);
            
            const pane = document.querySelector('.editor-pane');
            pane.classList.add('drag-over');
            
            pane.dispatchEvent(new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer }));
            pane.dispatchEvent(new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer }));
            pane.dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer }));
        }, zipBytes.toString('base64'));

        // 5. Wait for the load to finish and assert the restored state
        await page.waitForFunction(() => {
            return window._turiTabs && window._turiTabs.tabs().length === 3;
        });

        const loadedTabs = await page.evaluate(() => window._turiTabs.tabs());
        const activeId = await page.evaluate(() => window._turiTabs.activeId());
        const activeContent = await page.evaluate(() => window._turiEditor.getValue());

        // Assert names and active tab match the originally downloaded workspace.
        expect(loadedTabs.map(t => t.name)).toEqual(['main.tur', 'util.tur', 'math.tur']);
        expect(activeId).toBe(mathId);
        expect(activeContent).toBe('(defn cube [x] (* x (* x x)))');

        // Check each tab's restored content
        const mainTab = loadedTabs.find(t => t.name === 'main.tur');
        const utilTab = loadedTabs.find(t => t.name === 'util.tur');
        expect(mainTab.content).toBe('(println "hello main")');
        expect(utilTab.content).toBe('(defn square [x] (* x x))');
    });
});
