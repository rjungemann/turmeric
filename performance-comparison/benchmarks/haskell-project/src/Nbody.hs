-- nbody -- LCG-initialized bodies over mutable unboxed doubles, pairwise
-- forces, kinetic-energy checksum at %.4f.  Init casts the LCG's top 32
-- bits through SIGNED 64-bit like the C/Turmeric column.
{-# LANGUAGE BangPatterns #-}
import System.Environment (getArgs)
import Text.Printf (printf)
import Foreign.Marshal.Alloc (mallocBytes, free)
import Foreign.Ptr (Ptr)
import Foreign.Storable (peekElemOff, pokeElemOff)
import Data.Word (Word64)
import Data.Int (Int64)
lcgA, lcgC :: Word64
lcgA = 6364136223846793005
lcgC = 1442695040888963407
-- layout: 7 doubles per body: x y z vx vy vz mass
main :: IO ()
main = do
  args <- getArgs
  let (nBodies, steps) = case args of
        (a:b:_) -> (read a, read b)
        (a:_)   -> (read a, 1000)
        _       -> (5, 1000) :: (Int, Int)
  p <- mallocBytes (nBodies * 7 * 8) :: IO (Ptr Double)
  let lcgF s = fromIntegral (fromIntegral (s `div` 4294967296) :: Int64) / 1e8
      initB !i !st
        | i >= nBodies = return ()
        | otherwise = do
            let s1 = st * lcgA + lcgC
                s2 = s1 * lcgA + lcgC
                s3 = s2 * lcgA + lcgC
            pokeElemOff p (i * 7 + 0) (lcgF s1)
            pokeElemOff p (i * 7 + 1) (lcgF s2)
            pokeElemOff p (i * 7 + 2) (lcgF s3)
            pokeElemOff p (i * 7 + 3) 0.0
            pokeElemOff p (i * 7 + 4) 0.0
            pokeElemOff p (i * 7 + 5) 0.0
            pokeElemOff p (i * 7 + 6) (1.0 + fromIntegral (i `mod` 5) * 0.5)
            initB (i + 1) s3
  initB 0 42
  let pair !i !j
        | j >= nBodies = return ()
        | otherwise = do
            xi <- peekElemOff p (i*7+0); yi <- peekElemOff p (i*7+1); zi <- peekElemOff p (i*7+2)
            xj <- peekElemOff p (j*7+0); yj <- peekElemOff p (j*7+1); zj <- peekElemOff p (j*7+2)
            mi <- peekElemOff p (i*7+6); mj <- peekElemOff p (j*7+6)
            let dx = xj - xi; dy = yj - yi; dz = zj - zi
                dist = sqrt (dx*dx + dy*dy + dz*dz) + 1e-10
                f = mi * mj / (dist * dist * dist)
            vxi <- peekElemOff p (i*7+3); pokeElemOff p (i*7+3) (vxi + f*dx)
            vyi <- peekElemOff p (i*7+4); pokeElemOff p (i*7+4) (vyi + f*dy)
            vzi <- peekElemOff p (i*7+5); pokeElemOff p (i*7+5) (vzi + f*dz)
            vxj <- peekElemOff p (j*7+3); pokeElemOff p (j*7+3) (vxj - f*dx)
            vyj <- peekElemOff p (j*7+4); pokeElemOff p (j*7+4) (vyj - f*dy)
            vzj <- peekElemOff p (j*7+5); pokeElemOff p (j*7+5) (vzj - f*dz)
            pair i (j + 1)
      move !i
        | i >= nBodies = return ()
        | otherwise = do
            x <- peekElemOff p (i*7+0); vx <- peekElemOff p (i*7+3)
            y <- peekElemOff p (i*7+1); vy <- peekElemOff p (i*7+4)
            z <- peekElemOff p (i*7+2); vz <- peekElemOff p (i*7+5)
            pokeElemOff p (i*7+0) (x + vx)
            pokeElemOff p (i*7+1) (y + vy)
            pokeElemOff p (i*7+2) (z + vz)
            move (i + 1)
      step !s
        | s >= steps = return ()
        | otherwise = do
            sequence_ [pair i (i + 1) | i <- [0 .. nBodies - 1]]
            move 0
            step (s + 1)
  step 0
  let ke !i !acc
        | i >= nBodies = return acc
        | otherwise = do
            vx <- peekElemOff p (i*7+3); vy <- peekElemOff p (i*7+4)
            vz <- peekElemOff p (i*7+5); m <- peekElemOff p (i*7+6)
            ke (i + 1) (acc + 0.5 * m * (vx*vx + vy*vy + vz*vz))
  e <- ke 0 0.0
  free p
  printf "%.4f\n" e
