module "SpriteAgent";

new SceneToy() {
  new SpritePlayer(PatrolSprite) {
    position = "0 0";
    layer = 1;
    sortPoint = 5;
    physicsEnabled = true;
    collisionRadius = 0.4;
  };
};
