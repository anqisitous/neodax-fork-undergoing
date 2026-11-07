// ae/create_comp.jsx — create a composition in After Effects.
// Run in AE:  osascript -e 'tell application "Adobe After Effects 2026" to DoScriptFile (POSIX file "/abs/path/ae/create_comp.jsx")'
// ExtendScript (ES3) only: no let/const/arrow.
(function () {
    var LOG = null;

    function log(msg) {
        try { $.writeln(msg); } catch (e) {}
    }

    var CFG = {
        name: "Comp 1",
        width: 1920,
        height: 1080,
        pixelAspect: 1,
        durationSeconds: 10,
        frameRate: 30
    };

    function fail(msg) {
        log("ERROR: " + msg);
        return "ERROR: " + msg;
    }

    log("--- begin; AE " + app.version + " ---");

    try {
        var proj = app.project;
        if (!proj) {
            log("no project; calling app.newProject()");
            proj = app.newProject();
            if (!proj) {
                return fail("could not create a project");
            }
        }

        var comp = null;
        for (var i = 1; i <= proj.numItems; i++) {
            var it = proj.item(i);
            if (it instanceof CompItem && it.name === CFG.name) {
                comp = it;
                break;
            }
        }

        var created = false;
        if (!comp) {
            log("addComp " + CFG.name);
            comp = proj.items.addComp(
                CFG.name,
                CFG.width,
                CFG.height,
                CFG.pixelAspect,
                CFG.durationSeconds,
                CFG.frameRate
            );
            created = true;
        }

        if (!comp) {
            return fail("addComp returned null");
        }

        comp.openInViewer();

        var out = "OK: " + (created ? "created" : "reused") +
            " comp '" + comp.name + "' " +
            comp.width + "x" + comp.height +
            " @" + comp.frameRate + "fps, " +
            comp.duration + "s";
        log(out);
        return out;
    } catch (e) {
        return fail("exception: " + e.toString() + " line " + e.line);
    }
})();
