# FuseUIApp — "Application (FUSE UI)" project template

A `project.json` schema 2 app project (`"kind": "app"`): one 800×600 window showing `ui/main.fuseui`, on-demand redraw, no 3D / 2D worlds. The editor's New Project wizard ("Application (FUSE UI)") and `fuse_project_tool --new-app <dir>` write the same two files (`fuse::project::createAppProject`); the `fuse_project_schema2` gate checks they match.

Run it through the minimal init path (fuse_app shell + `AppPresent`, no renderer world, physics or job workers):

```
fuse_app_host Templates/FuseUIApp
```

See `docs/ui/README.md` ("App projects") for the schema.
