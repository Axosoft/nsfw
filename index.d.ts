declare module 'nsfw' {
    /** Returns a Promise that resolves to the created NSFW Object.
     * @param {watchPath} watchPath - the path that nsfw should watchPath
     * @param {eventCallback} eventCallback - callback that will be fired when NSFW has change events
     * @param {options} options - options
     */
    interface NsfwFunction {
        (watchPath: string, eventCallback: (events: Array<nsfw.ChangeEvent>) => void, options: nsfw.Options): Promise<nsfw.NSFW>;
        actions: {
            CREATED: number;
			DELETED: number;
			MODIFIED: number;
			RENAMED: number;
        };
    }
    
    namespace nsfw {
        interface Options {
            /** time in milliseconds to debounce the event callback */
            debounceMS?: number;
            /**  callback to fire in the case of errors */
            errorCallback: (err: any) => void
        }

        interface ChangeEvent {
            /** the type of event that occurred */
            action: number;
            /** the location the event took place */
            directory: string;
            /** the name of the file that was changed(Not available for rename events) */
            file: string;
            /**  the name of the file before a rename(Only available for rename events)*/
            oldFile?: string;
            /** the new location of the file(Only available for rename events, only useful on linux) */
            newDirectory?: string;
            /** the name of the file after a rename(Only available for rename events) */
            newFile?: string;
        }

        interface NSFW {
            /** Returns a Promise that resolves when the NSFW object has started watching the path. */
            start: () => Promise<void>;
            /** Returns a Promise that resolves when NSFW object has halted. */
            stop: () => Promise<void>;
        }
    }
    var func: NsfwFunction;
    export = func;
}