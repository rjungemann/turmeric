-- ray_tracing -- three-sphere diffuse tracer, integer checksum.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
spheres :: [(Double, Double, Double, Double)]
spheres = [(0,0,-5,1.0), (2,0,-7,1.5), (-3,0,-6,0.8)]
dot3 :: (Double,Double,Double) -> (Double,Double,Double) -> Double
dot3 (a1,a2,a3) (b1,b2,b3) = a1*b1 + a2*b2 + a3*b3
norm3 :: (Double,Double,Double) -> (Double,Double,Double)
norm3 v@(x,y,z) = let l = sqrt (dot3 v v) + 1e-15 in (x/l, y/l, z/l)
sphereHit :: (Double,Double,Double,Double) -> (Double,Double,Double) -> Maybe Double
sphereHit (cx,cy,cz,r) rd =
  let oc = (-cx, -cy, -cz)
      a = dot3 rd rd
      b2 = dot3 oc rd
      c = dot3 oc oc - r*r
      d = b2*b2 - a*c
  in if d < 0 then Nothing
     else let t = (-b2 - sqrt d) / a
          in if t > 0.001 then Just t else Nothing
main :: IO ()
main = do
  args <- getArgs
  let (width, height) = case args of
        (a:b:_) -> (read a, read b)
        (a:_)   -> (read a, 75)
        _       -> (100, 75) :: (Int, Int)
      light = norm3 (1, 1, -1)
      cell !y !x =
        let u = fromIntegral x / fromIntegral width * 2 - 1
            v = fromIntegral y / fromIntegral height * 2 - 1
            rd = norm3 (u, v, -1)
            best = foldr (\s acc -> case sphereHit s rd of
                                      Just t | maybe True (\(bt,_) -> t < bt) acc -> Just (t, s)
                                      _ -> acc) Nothing spheres
        in case best of
             Nothing -> 0
             Just (t, (sx,sy,sz,_)) ->
               let hp = let (rx,ry,rz) = rd in (rx*t, ry*t, rz*t)
                   n = norm3 (let (hx,hy,hz) = hp in (hx-sx, hy-sy, hz-sz))
                   diff = max 0 (dot3 n light)
               in truncate (diff * 255) :: Int
      total = sum [cell y x | y <- [0 .. height - 1], x <- [0 .. width - 1]]
  print total
