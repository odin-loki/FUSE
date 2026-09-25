"""FUSE WorldExtract: world-model video -> 3D world -> FUSE POCO assets.

Modules:
    common     logging, hashing, stage cache, dependency checks
    actions    Matrix-Game 2.0 action format, reconstruction-friendly trajectory presets, pose priors
    geometry   rotations, Sim(3) alignment, plane RANSAC, metrics
    stages     one module per `extract.py` pipeline stage
"""

__version__ = "0.1.0"
