using System.Numerics;
using Shinkou.Engine.Scripting;

namespace Shinkou.Samples;

public sealed class PlayerController : ScriptComponent
{
    protected override void OnUpdate(float deltaSeconds)
    {
        var position = GameObject.Transform.Position;
        GameObject.Transform.Position = position + new Vector3(deltaSeconds, 0, 0);
    }
}
